#include "PostIt.h"
#include <windowsx.h>
#include <dwmapi.h>
#include <algorithm>
#include <cmath>

#pragma comment(lib, "dwmapi.lib")

namespace
{
    constexpr wchar_t kClassName[] = L"ScreenshotCropAndStorePostItClass";

    // Contour fin de 1 px : sans lui, un post-it clair sur un fond clair serait invisible.
    // Mettre à false pour une image strictement sans aucun bord.
    constexpr bool kDrawThinBorder = true;
    constexpr COLORREF kBorderColor = RGB(70, 70, 70);

    constexpr double kMinOpacity = 0.15;       // opacité minimale (Alt + molette)
    constexpr double kOpacityPerNotch = 0.10;  // variation d'opacité par cran de molette
    constexpr double kZoomPerNotch = 1.10;     // facteur de zoom par cran de molette
    constexpr int kGripAt96Dpi = 8;            // épaisseur de la zone de redimensionnement (px à 96 DPI)

    // Attributs DWM (valeurs recopiées ici pour ne pas dépendre de la version du SDK).
    constexpr DWORD kDwmCornerPreference = 33;     // DWMWA_WINDOW_CORNER_PREFERENCE
    constexpr DWORD kDwmBorderColor = 34;          // DWMWA_BORDER_COLOR
    constexpr int kDwmDoNotRound = 1;              // DWMWCP_DONOTROUND
    constexpr DWORD kDwmColorNone = 0xFFFFFFFE;    // DWMWA_COLOR_NONE

    enum : UINT
    {
        kCmdClose = 1,
        kCmdTopmost,
        kCmdActualSize,
        kCmdClickThrough
    };

    struct Limits
    {
        int minW, minH, maxW, maxH;
    };

    struct PostItWindow
    {
        std::shared_ptr<const PostItImage> image;
        double opacity = 1.0;
        bool liveSizing = false;   // true pendant un redimensionnement à la souris (rendu rapide)
        bool dragging = false;
        POINT dragOffset{};
    };

    std::vector<HWND> g_postIts;
    bool g_clickThrough = false;

    // Bornes de taille de fenêtre en respectant le ratio : zoom de 10 % à 800 %, sans jamais
    // dépasser 16384 px ni descendre sous ~16 px pour la plus petite dimension.
    Limits ComputeLimits(const PostItImage& img)
    {
        double minDim = static_cast<double>((std::min)(img.width, img.height));
        double maxDim = static_cast<double>((std::max)(img.width, img.height));

        double minScale = (std::min)(1.0, (std::max)(0.1, 16.0 / minDim));
        double maxScale = (std::max)(1.0, (std::min)(8.0, 16384.0 / maxDim));

        Limits l;
        l.minW = (std::max)(1, static_cast<int>(std::lround(img.width * minScale)));
        l.minH = (std::max)(1, static_cast<int>(std::lround(img.height * minScale)));
        l.maxW = (std::max)(l.minW, static_cast<int>(std::lround(img.width * maxScale)));
        l.maxH = (std::max)(l.minH, static_cast<int>(std::lround(img.height * maxScale)));
        return l;
    }

    int HeightForWidth(const PostItImage& img, int w)
    {
        return (std::max)(1, static_cast<int>(std::lround(w * static_cast<double>(img.height) / img.width)));
    }

    void ApplyClickThrough(HWND hwnd, bool enabled)
    {
        LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if (enabled) ex |= WS_EX_TRANSPARENT;
        else ex &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
    }

    void ApplyOpacity(HWND hwnd, const PostItWindow& s)
    {
        BYTE alpha = static_cast<BYTE>(std::lround(s.opacity * 255.0));
        SetLayeredWindowAttributes(hwnd, 0, alpha, LWA_ALPHA);
    }

    // Zoom à la molette : la fenêtre grandit/rétrécit autour du curseur (le point visé reste
    // sous la souris), le ratio de l'image est toujours conservé.
    void ZoomAt(HWND hwnd, PostItWindow& s, int wheelDelta, POINT screenPt)
    {
        Limits lim = ComputeLimits(*s.image);

        RECT r; GetWindowRect(hwnd, &r);
        int oldW = r.right - r.left;
        int oldH = r.bottom - r.top;
        if (oldW <= 0 || oldH <= 0) return;

        double factor = std::pow(kZoomPerNotch, wheelDelta / 120.0);
        int newW = static_cast<int>(std::lround(oldW * factor));
        if (newW == oldW) newW += (wheelDelta > 0 ? 1 : -1); // garantit une progression sur les petites tailles
        newW = std::clamp(newW, lim.minW, lim.maxW);
        int newH = HeightForWidth(*s.image, newW);
        if (newW == oldW && newH == oldH) return;

        double fx = std::clamp((screenPt.x - r.left) / static_cast<double>(oldW), 0.0, 1.0);
        double fy = std::clamp((screenPt.y - r.top) / static_cast<double>(oldH), 0.0, 1.0);
        int x = static_cast<int>(std::lround(screenPt.x - fx * newW));
        int y = static_cast<int>(std::lround(screenPt.y - fy * newH));

        SetWindowPos(hwnd, nullptr, x, y, newW, newH, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // Taille réelle : 1 pixel de l'image = 1 pixel physique de l'écran. Centré sur la position actuelle.
    void SetActualSize(HWND hwnd, PostItWindow& s)
    {
        Limits lim = ComputeLimits(*s.image);

        RECT r; GetWindowRect(hwnd, &r);
        int oldW = r.right - r.left;
        int oldH = r.bottom - r.top;

        int w = std::clamp(s.image->width, lim.minW, lim.maxW);
        int h = HeightForWidth(*s.image, w);
        int x = r.left + (oldW - w) / 2;
        int y = r.top + (oldH - h) / 2;

        SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void ShowContextMenu(HWND hwnd, PostItWindow& s)
    {
        bool isTopmost = (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;

        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING | (isTopmost ? MF_CHECKED : 0), kCmdTopmost, L"Toujours au premier plan");
        AppendMenuW(menu, MF_STRING, kCmdActualSize, L"Afficher à 100 %");
        AppendMenuW(menu, MF_STRING | (g_clickThrough ? MF_CHECKED : 0), kCmdClickThrough,
            L"Mode traversable (se désactive via l'icône de la barre des tâches)");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kCmdClose, L"Fermer");

        POINT pt; GetCursorPos(&pt);
        SetForegroundWindow(hwnd); // nécessaire pour que le menu se referme correctement
        UINT cmd = static_cast<UINT>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr));
        PostMessageW(hwnd, WM_NULL, 0, 0);
        DestroyMenu(menu);

        switch (cmd)
        {
        case kCmdTopmost:
            SetWindowPos(hwnd, isTopmost ? HWND_NOTOPMOST : HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            break;
        case kCmdActualSize:
            SetActualSize(hwnd, s);
            break;
        case kCmdClickThrough:
            SetPostItsClickThrough(!g_clickThrough);
            break;
        case kCmdClose:
            DestroyWindow(hwnd); // 's' est libéré dans WM_NCDESTROY : ne plus l'utiliser après
            break;
        }
    }

    // Zones de redimensionnement sur les bords/coins ; l'intérieur sert au déplacement.
    LRESULT HitTest(HWND hwnd, LPARAM lParam)
    {
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT rc; GetWindowRect(hwnd, &rc);

        int grip = MulDiv(kGripAt96Dpi, static_cast<int>(GetDpiForWindow(hwnd)), 96);
        grip = (std::max)(3, (std::min)(grip, static_cast<int>((std::min)(rc.right - rc.left, rc.bottom - rc.top) / 3)));

        bool left = pt.x < rc.left + grip;
        bool right = pt.x >= rc.right - grip;
        bool top = pt.y < rc.top + grip;
        bool bottom = pt.y >= rc.bottom - grip;

        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
        return HTCLIENT;
    }

    void Paint(HWND hwnd, const PostItWindow& s)
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rc; GetClientRect(hwnd, &rc);
        const PostItImage& img = *s.image;

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = img.width;
        bmi.bmiHeader.biHeight = -img.height; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        // Au-delà de 200 % (ou pendant un redimensionnement à la souris) : pixels nets, sans
        // lissage. Sinon rendu lissé de qualité (réduction notamment).
        double scale = static_cast<double>(rc.right) / img.width;
        SetStretchBltMode(hdc, (s.liveSizing || scale > 2.0) ? COLORONCOLOR : HALFTONE);
        SetBrushOrgEx(hdc, 0, 0, nullptr);

        StretchDIBits(hdc, 0, 0, rc.right, rc.bottom,
            0, 0, img.width, img.height,
            img.pixels.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);

        if (kDrawThinBorder)
        {
            HBRUSH brush = CreateSolidBrush(kBorderColor);
            FrameRect(hdc, &rc, brush);
            DeleteObject(brush);
        }

        EndPaint(hwnd, &ps);
    }

    LRESULT CALLBACK PostItWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        PostItWindow* self = reinterpret_cast<PostItWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        switch (msg)
        {
        case WM_NCCREATE:
        {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            break;
        }

        // Pas de zone non cliente : toute la fenêtre est l'image (WS_THICKFRAME n'est là que
        // pour que Windows gère proprement le redimensionnement par les bords).
        case WM_NCCALCSIZE:
            return 0;

        case WM_NCHITTEST:
            return HitTest(hwnd, lParam);

        case WM_GETMINMAXINFO:
            if (self)
            {
                Limits lim = ComputeLimits(*self->image);
                auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
                mmi->ptMinTrackSize.x = lim.minW;
                mmi->ptMinTrackSize.y = lim.minH;
                mmi->ptMaxTrackSize.x = lim.maxW;
                mmi->ptMaxTrackSize.y = lim.maxH;
                return 0;
            }
            break;

        // Redimensionnement par les bords : on impose le ratio de l'image.
        case WM_SIZING:
            if (self)
            {
                RECT* r = reinterpret_cast<RECT*>(lParam);
                const PostItImage& img = *self->image;
                Limits lim = ComputeLimits(img);
                int edge = static_cast<int>(wParam);

                bool leftEdge = (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT);
                bool topEdge = (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT);
                bool heightDriven = (edge == WMSZ_TOP || edge == WMSZ_BOTTOM);

                int w = r->right - r->left;
                int h = r->bottom - r->top;
                if (heightDriven)
                    w = static_cast<int>(std::lround(h * static_cast<double>(img.width) / img.height));

                w = std::clamp(w, lim.minW, lim.maxW);
                h = HeightForWidth(img, w);

                if (leftEdge) r->left = r->right - w; else r->right = r->left + w;
                if (topEdge) r->top = r->bottom - h; else r->bottom = r->top + h;
                return TRUE;
            }
            break;

        case WM_ENTERSIZEMOVE:
            if (self) self->liveSizing = true;
            return 0;

        case WM_EXITSIZEMOVE:
            if (self) self->liveSizing = false;
            InvalidateRect(hwnd, nullptr, FALSE); // repeint en qualité complète
            return 0;

        case WM_SIZE:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        // On garde la taille en pixels quand la fenêtre passe sur un écran d'échelle différente.
        case WM_DPICHANGED:
            return 0;

        // Empêche le double-clic sur un bord (agrandissement vertical Windows) et le
        // maximiser/minimiser, qui casseraient le ratio.
        case WM_NCLBUTTONDBLCLK:
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xFFF0) == SC_MAXIMIZE || (wParam & 0xFFF0) == SC_MINIMIZE) return 0;
            break;

        // Alt seul ne doit pas déclencher le mode "menu" de Windows (on utilise Alt + molette).
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
            if (wParam == VK_MENU) return 0;
            break;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            if (self)
            {
                Paint(hwnd, *self);
                return 0;
            }
            break;

        // Déplacement : glisser avec le bouton gauche.
        case WM_LBUTTONDOWN:
            if (self)
            {
                POINT p; GetCursorPos(&p);
                RECT r; GetWindowRect(hwnd, &r);
                self->dragOffset = { p.x - r.left, p.y - r.top };
                self->dragging = true;
                SetCapture(hwnd);
            }
            return 0;

        case WM_MOUSEMOVE:
            if (self && self->dragging)
            {
                POINT p; GetCursorPos(&p);
                SetWindowPos(hwnd, nullptr, p.x - self->dragOffset.x, p.y - self->dragOffset.y, 0, 0,
                    SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;

        case WM_LBUTTONUP:
            if (self && self->dragging)
            {
                self->dragging = false;
                ReleaseCapture();
            }
            return 0;

        case WM_CAPTURECHANGED:
            if (self) self->dragging = false;
            return 0;

        case WM_RBUTTONUP:
            if (self) ShowContextMenu(hwnd, *self);
            return 0;

        // Molette : zoom. Alt + molette : opacité.
        case WM_MOUSEWHEEL:
            if (self)
            {
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                if (GetKeyState(VK_MENU) & 0x8000)
                {
                    self->opacity = std::clamp(self->opacity + kOpacityPerNotch * delta / 120.0, kMinOpacity, 1.0);
                    ApplyOpacity(hwnd, *self);
                }
                else
                {
                    POINT p{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                    ZoomAt(hwnd, *self, delta, p);
                }
            }
            return 0;

        case WM_NCDESTROY:
            g_postIts.erase(std::remove(g_postIts.begin(), g_postIts.end(), hwnd), g_postIts.end());
            delete self;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            break;
        }

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    bool EnsureClassRegistered(HINSTANCE hInstance)
    {
        static bool registered = false;
        if (registered) return true;

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = PostItWndProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;

        registered = RegisterClassExW(&wc) != 0;
        return registered;
    }
}

void OpenPostIt(HINSTANCE hInstance, std::shared_ptr<const PostItImage> image)
{
    if (!image || image->width <= 0 || image->height <= 0 || image->pixels.empty()) return;
    if (!EnsureClassRegistered(hInstance)) return;

    // Position : là où la capture a été prise, avec un léger décalage en cascade si d'autres
    // post-its sont déjà ouverts (pour ne pas les empiler exactement).
    int cascade = 28 * static_cast<int>(g_postIts.size() % 8);
    POINT origin{ image->screenOrigin.x + cascade, image->screenOrigin.y + cascade };

    HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(monitor, &mi);
    const RECT wa = mi.rcWork;

    // Taille initiale : 100 %, réduite (ratio conservé) si l'image dépasse la zone de travail.
    Limits lim = ComputeLimits(*image);
    double fit = (std::min)(0.95 * (wa.right - wa.left) / image->width,
                            0.95 * (wa.bottom - wa.top) / image->height);
    int w = image->width;
    if (fit < 1.0) w = static_cast<int>(std::lround(image->width * fit));
    w = std::clamp(w, lim.minW, lim.maxW);
    int h = HeightForWidth(*image, w);

    int x = origin.x;
    int y = origin.y;
    if (x + w > wa.right) x = wa.right - w;
    if (y + h > wa.bottom) y = wa.bottom - h;
    if (x < wa.left) x = wa.left;
    if (y < wa.top) y = wa.top;

    auto* state = new PostItWindow();
    state->image = std::move(image);

    DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED; // TOOLWINDOW : ni barre des tâches ni Alt-Tab
    if (g_clickThrough) exStyle |= WS_EX_TRANSPARENT;

    HWND hwnd = CreateWindowExW(exStyle, kClassName, L"Post-it", WS_POPUP | WS_THICKFRAME,
        x, y, w, h, nullptr, nullptr, hInstance, state);
    if (!hwnd)
    {
        delete state;
        return;
    }

    ApplyOpacity(hwnd, *state); // indispensable : une fenêtre "layered" n'apparaît qu'après cet appel

    // Windows 11 : pas de coins arrondis ni de liseré de couleur d'accentuation autour de l'image.
    int cornerPref = kDwmDoNotRound;
    DwmSetWindowAttribute(hwnd, kDwmCornerPreference, &cornerPref, sizeof(cornerPref));
    DWORD borderColor = kDwmColorNone;
    DwmSetWindowAttribute(hwnd, kDwmBorderColor, &borderColor, sizeof(borderColor));

    g_postIts.push_back(hwnd);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
}

void CloseAllPostIts()
{
    std::vector<HWND> copy = g_postIts; // WM_NCDESTROY modifie g_postIts pendant l'itération
    for (HWND h : copy) DestroyWindow(h);
}

int PostItCount()
{
    return static_cast<int>(g_postIts.size());
}

bool PostItsClickThrough()
{
    return g_clickThrough;
}

void SetPostItsClickThrough(bool enabled)
{
    g_clickThrough = enabled;
    for (HWND h : g_postIts) ApplyClickThrough(h, enabled);
}
