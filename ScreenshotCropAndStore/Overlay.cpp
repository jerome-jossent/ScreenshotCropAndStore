#include "Overlay.h"
#include <windowsx.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <cstdio>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

using Microsoft::WRL::ComPtr;

namespace
{
    // --- Loupe ---
    constexpr int MagnifierSourceSize = 32;   // pixels de l'image d'origine échantillonnés
    constexpr int MagnifierZoom = 8;          // facteur d'agrandissement
    constexpr int MagnifierDisplaySize = MagnifierSourceSize * MagnifierZoom;
    constexpr int MagnifierOffset = 24;       // écart entre le curseur et la loupe
    constexpr int MagnifierLabelHeight = 22;  // hauteur réservée au texte de coordonnées

    // --- Voile sombre --- (0 = image d'origine, 1 = noir complet ; ~ intensite_lumineuse=65)
    constexpr float VeilAlpha = 0.35f;

    struct OverlayState
    {
        ComPtr<ID2D1Factory> d2dFactory;
        ComPtr<ID2D1HwndRenderTarget> renderTarget;
        ComPtr<ID2D1Bitmap> screenBitmap;
        ComPtr<IDWriteFactory> dwriteFactory;
        ComPtr<IDWriteTextFormat> textFormat;

        ComPtr<ID2D1SolidColorBrush> whiteBrush;
        ComPtr<ID2D1SolidColorBrush> blackVeilBrush;
        ComPtr<ID2D1SolidColorBrush> labelBgBrush;
        ComPtr<ID2D1SolidColorBrush> selectionBorderBrush;
        ComPtr<ID2D1SolidColorBrush> crosshairBrush;
        ComPtr<ID2D1SolidColorBrush> magnifierBrush;

        const CapturedImage* capture = nullptr;

        POINT startPoint{};
        RECT currentSelection{};
        bool isSelecting = false;

        POINT lastMouse{};       // position "effective" utilisée pour le rendu (mode précision inclus)
        POINT rawLastMouse{};    // dernière position brute du curseur système, pour calculer les deltas
        float effectiveX = 0.0f;
        float effectiveY = 0.0f;
        bool hasMouse = false;
        bool wasPrecisionMode = false; // pour détecter le relâchement de Ctrl

        RECT outSelection{};
        bool accepted = false;
        bool done = false;
    };

    RECT NormalizedRect(POINT a, POINT b)
    {
        RECT r;
        r.left = (std::min)(a.x, b.x);
        r.top = (std::min)(a.y, b.y);
        r.right = (std::max)(a.x, b.x);
        r.bottom = (std::max)(a.y, b.y);
        return r;
    }

    bool RectHasArea(const RECT& r) { return r.right > r.left && r.bottom > r.top; }

    D2D1_RECT_F ToD2DRect(const RECT& r)
    {
        return D2D1::RectF(static_cast<float>(r.left), static_cast<float>(r.top),
            static_cast<float>(r.right), static_cast<float>(r.bottom));
    }

    void EnsureDeviceResources(HWND hwnd, OverlayState& state)
    {
        if (state.renderTarget) return;

        RECT rc; GetClientRect(hwnd, &rc);
        D2D1_SIZE_U size = D2D1::SizeU(
            static_cast<UINT32>(rc.right - rc.left), static_cast<UINT32>(rc.bottom - rc.top));

        // Important : on force 96 DPI ici (1 DIP = 1 pixel physique). Sans ça, Direct2D utilise
        // le DPI système réel (ex. 144 à 150%) pour interpréter nos coordonnées, alors que tout
        // notre code (position souris, pixels du bitmap capturé) raisonne déjà en pixels
        // physiques bruts — d'où une image "zoomée" et un décalage curseur/rendu proportionnel
        // à l'échelle d'affichage.
        D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_UNKNOWN),
            96.0f, 96.0f);

        state.d2dFactory->CreateHwndRenderTarget(
            rtProps,
            D2D1::HwndRenderTargetProperties(hwnd, size),
            &state.renderTarget);

        if (!state.renderTarget) return;
        auto* rt = state.renderTarget.Get();

        D2D1_BITMAP_PROPERTIES bmpProps = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));

        rt->CreateBitmap(
            D2D1::SizeU(static_cast<UINT32>(state.capture->width), static_cast<UINT32>(state.capture->height)),
            state.capture->bits,
            static_cast<UINT32>(state.capture->strideBytes),
            bmpProps,
            &state.screenBitmap);

        rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &state.whiteBrush);
        rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black, VeilAlpha), &state.blackVeilBrush);
        rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black, 0.78f), &state.labelBgBrush);
        rt->CreateSolidColorBrush(D2D1::ColorF(0.117f, 0.565f, 1.0f), &state.selectionBorderBrush); // DeepSkyBlue
        rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &state.crosshairBrush);
        rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Red), &state.magnifierBrush);

        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(state.dwriteFactory.GetAddressOf()));
        state.dwriteFactory->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"", &state.textFormat);
    }

    RECT GetMagnifierBox(POINT cursor, RECT clientRect)
    {
        int size = MagnifierDisplaySize;
        int x = cursor.x + MagnifierOffset;
        int y = cursor.y + MagnifierOffset;

        if (x + size > clientRect.right) x = cursor.x - MagnifierOffset - size;
        if (y + size + MagnifierLabelHeight > clientRect.bottom) y = cursor.y - MagnifierOffset - size;

        x = (std::max)(static_cast<int>(clientRect.left), x);
        y = (std::max)(static_cast<int>(clientRect.top), y);

        return { x, y, x + size, y + size };
    }

    void DrawTextLabel(OverlayState& state, const std::wstring& text, float x, float y)
    {
        auto* rt = state.renderTarget.Get();

        ComPtr<IDWriteTextLayout> layout;
        state.dwriteFactory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
            state.textFormat.Get(), 400.0f, 40.0f, &layout);
        if (!layout) return;

        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);

        D2D1_RECT_F bg = D2D1::RectF(x, y, x + metrics.width + 8, y + metrics.height + 4);
        rt->FillRectangle(&bg, state.labelBgBrush.Get());
        rt->DrawTextLayout(D2D1::Point2F(x + 4, y + 2), layout.Get(), state.whiteBrush.Get());
    }

    void DrawCrosshair(OverlayState& state, RECT clientRect, POINT cursor)
    {
        auto* rt = state.renderTarget.Get();
        float x = static_cast<float>(cursor.x);
        float y = static_cast<float>(cursor.y);
        rt->DrawLine(D2D1::Point2F(x, static_cast<float>(clientRect.top)),
            D2D1::Point2F(x, static_cast<float>(clientRect.bottom)), state.crosshairBrush.Get(), 1.0f);
        rt->DrawLine(D2D1::Point2F(static_cast<float>(clientRect.left), y),
            D2D1::Point2F(static_cast<float>(clientRect.right), y), state.crosshairBrush.Get(), 1.0f);
    }

    void DrawMagnifier(OverlayState& state, RECT clientRect, POINT cursor)
    {
        auto* rt = state.renderTarget.Get();
        RECT box = GetMagnifierBox(cursor, clientRect);
        D2D1_RECT_F boxF = ToD2DRect(box);

        int half = MagnifierSourceSize / 2;
        int srcX = std::clamp((int)cursor.x - half, 0, (std::max)(0, state.capture->width - MagnifierSourceSize));
        int srcY = std::clamp((int)cursor.y - half, 0, (std::max)(0, state.capture->height - MagnifierSourceSize));
        D2D1_RECT_F srcRect = D2D1::RectF(static_cast<float>(srcX), static_cast<float>(srcY),
            static_cast<float>(srcX + MagnifierSourceSize), static_cast<float>(srcY + MagnifierSourceSize));

        // Nearest-neighbor : rendu net "pixel carré", pas de flou d'interpolation.
        rt->DrawBitmap(state.screenBitmap.Get(), &boxF, 1.0f,
            D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, &srcRect);

        float pixelSize = static_cast<float>(MagnifierDisplaySize) / MagnifierSourceSize;
        int pixelIndexX = cursor.x - srcX;
        int pixelIndexY = cursor.y - srcY;
        D2D1_RECT_F highlight = D2D1::RectF(
            boxF.left + pixelIndexX * pixelSize, boxF.top + pixelIndexY * pixelSize,
            boxF.left + (pixelIndexX + 1) * pixelSize, boxF.top + (pixelIndexY + 1) * pixelSize);

        rt->DrawRectangle(&highlight, state.magnifierBrush.Get(), 1.0f);

        // Grande croix de visée, avec un vide au centre pour ne pas recouvrir le pixel exact
        // (déjà signalé par le carré rouge ci-dessus). Épaisseur des traits = 1 pixel zoomé.
        float midX = (highlight.left + highlight.right) / 2;
        float midY = (highlight.top + highlight.bottom) / 2;

        rt->DrawLine(D2D1::Point2F(midX, boxF.top), D2D1::Point2F(midX, highlight.top),
            state.magnifierBrush.Get(), pixelSize);
        rt->DrawLine(D2D1::Point2F(midX, highlight.bottom), D2D1::Point2F(midX, boxF.bottom),
            state.magnifierBrush.Get(), pixelSize);
        rt->DrawLine(D2D1::Point2F(boxF.left, midY), D2D1::Point2F(highlight.left, midY),
            state.magnifierBrush.Get(), pixelSize);
        rt->DrawLine(D2D1::Point2F(highlight.right, midY), D2D1::Point2F(boxF.right, midY),
            state.magnifierBrush.Get(), pixelSize);

        rt->DrawRectangle(&boxF, state.whiteBrush.Get(), 2.0f);

        wchar_t coordText[64];
        swprintf_s(coordText, L"%d, %d px", cursor.x, cursor.y);
        DrawTextLabel(state, coordText, boxF.left, boxF.bottom + 2);
    }

    void Render(HWND hwnd, OverlayState& state)
    {
        EnsureDeviceResources(hwnd, state);
        if (!state.renderTarget) return;
        auto* rt = state.renderTarget.Get();

        RECT clientRect; GetClientRect(hwnd, &clientRect);

        rt->BeginDraw();

        D2D1_RECT_F fullRect = ToD2DRect(clientRect);
        rt->DrawBitmap(state.screenBitmap.Get(), &fullRect, 1.0f,
            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
        rt->FillRectangle(&fullRect, state.blackVeilBrush.Get());

        if (RectHasArea(state.currentSelection))
        {
            D2D1_RECT_F selRect = ToD2DRect(state.currentSelection);
            // Réaffiche la portion sélectionnée "en clair" (image d'origine, non assombrie).
            rt->DrawBitmap(state.screenBitmap.Get(), &selRect, 1.0f,
                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &selRect);
            rt->DrawRectangle(&selRect, state.selectionBorderBrush.Get(), 1.0f);

            wchar_t sizeText[64];
            swprintf_s(sizeText, L"%ld x %ld",
                state.currentSelection.right - state.currentSelection.left,
                state.currentSelection.bottom - state.currentSelection.top);
            float labelY = (std::max)(0.0f, selRect.top - 24);
            DrawTextLabel(state, sizeText, selRect.left, labelY);
        }

        if (state.hasMouse)
        {
            // Ordre important : les repères et la croix sont dessinés avant la loupe,
            // qui les recouvre donc naturellement dans sa propre zone.
            DrawCrosshair(state, clientRect, state.lastMouse);
            DrawMagnifier(state, clientRect, state.lastMouse);
        }

        HRESULT hr = rt->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET)
        {
            state.renderTarget.Reset();
            state.screenBitmap.Reset();
        }
    }

    LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        OverlayState* state = reinterpret_cast<OverlayState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

        switch (msg)
        {
        case WM_CREATE:
        {
            auto* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
            SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return 0;
        }
        case WM_ERASEBKGND:
            return 1; // évite le clignotement : Direct2D redessine tout lui-même
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            if (state) Render(hwnd, *state);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_SIZE:
            if (state && state->renderTarget)
            {
                state->renderTarget->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
            }
            return 0;
        case WM_LBUTTONDOWN:
            if (state)
            {
                state->isSelecting = true;
                // On repart de la position "effective" (déjà affichée), cohérente même si le
                // mode précision (Ctrl) était actif juste avant le clic.
                state->startPoint = state->lastMouse;
                state->currentSelection = { state->startPoint.x, state->startPoint.y,
                                             state->startPoint.x, state->startPoint.y };
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_MOUSEMOVE:
            if (state)
            {
                POINT rawPos{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                bool precisionMode = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

                if (!state->hasMouse)
                {
                    state->effectiveX = static_cast<float>(rawPos.x);
                    state->effectiveY = static_cast<float>(rawPos.y);
                    state->hasMouse = true;
                }
                else if (state->wasPrecisionMode && !precisionMode)
                {
                    // Ctrl vient d'être relâché : on recolle immédiatement le repère sur la
                    // position réelle du curseur, plutôt que de laisser le décalage accumulé
                    // pendant le mode précision.
                    state->effectiveX = static_cast<float>(rawPos.x);
                    state->effectiveY = static_cast<float>(rawPos.y);
                }
                else
                {
                    int dx = rawPos.x - state->rawLastMouse.x;
                    int dy = rawPos.y - state->rawLastMouse.y;

                    // Ctrl enfoncé = mode précision : le repère (croix/loupe/sélection) avance
                    // 10x plus lentement que le curseur système réel, pour un cadrage fin.
                    // Le curseur Windows continue de bouger à vitesse normale à l'écran ; seul
                    // notre repère de visée ralentit.
                    float factor = precisionMode ? 0.1f : 1.0f;
                    state->effectiveX += dx * factor;
                    state->effectiveY += dy * factor;
                }

                state->rawLastMouse = rawPos;
                state->wasPrecisionMode = precisionMode;

                RECT clientRect; GetClientRect(hwnd, &clientRect);
                state->effectiveX = std::clamp(state->effectiveX,
                    static_cast<float>(clientRect.left), static_cast<float>(clientRect.right - 1));
                state->effectiveY = std::clamp(state->effectiveY,
                    static_cast<float>(clientRect.top), static_cast<float>(clientRect.bottom - 1));

                POINT p{ static_cast<int>(std::lround(state->effectiveX)),
                         static_cast<int>(std::lround(state->effectiveY)) };
                state->lastMouse = p;

                if (state->isSelecting)
                {
                    state->currentSelection = NormalizedRect(state->startPoint, p);
                }

                // Redessin complet à chaque frame : le rendu GPU (Direct2D, vsync via EndDraw)
                // absorbe très largement ce coût, contrairement au rendu logiciel GDI+.
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP:
            if (state)
            {
                ReleaseCapture();
                state->isSelecting = false;
                state->outSelection = state->currentSelection;
                state->accepted = RectHasArea(state->outSelection);
                state->done = true;
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_KEYUP:
            if (state && wParam == VK_CONTROL && state->hasMouse)
            {
                // Relâchement de Ctrl détecté sans mouvement de souris entre-temps : on force
                // quand même la resynchronisation immédiate du repère sur le curseur réel.
                POINT p; GetCursorPos(&p);
                ScreenToClient(hwnd, &p);
                state->effectiveX = static_cast<float>(p.x);
                state->effectiveY = static_cast<float>(p.y);
                state->rawLastMouse = p;
                state->wasPrecisionMode = false;
                state->lastMouse = p;
                if (state->isSelecting)
                {
                    state->currentSelection = NormalizedRect(state->startPoint, p);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_KEYDOWN:
            if (state && wParam == VK_ESCAPE)
            {
                state->accepted = false;
                state->done = true;
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_DESTROY:
            if (state) state->done = true;
            return 0;
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

bool ShowSelectionOverlay(HINSTANCE hInstance, const CapturedImage& capture, const RECT& virtualBounds, RECT& outSelection)
{
    static bool classRegistered = false;
    const wchar_t* className = L"ScreenshotCropAndStoreOverlayClass";
    if (!classRegistered)
    {
        WNDCLASS wc{};
        wc.lpfnWndProc = OverlayWndProc;
        wc.hInstance = hInstance;
        wc.lpszClassName = className;
        wc.hCursor = LoadCursor(nullptr, IDC_CROSS);
        RegisterClass(&wc);
        classRegistered = true;
    }

    OverlayState state;
    state.capture = &capture;

    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory),
        reinterpret_cast<void**>(state.d2dFactory.GetAddressOf()));

    HWND hwnd = CreateWindowEx(
        WS_EX_TOPMOST, className, L"", WS_POPUP,
        virtualBounds.left, virtualBounds.top,
        virtualBounds.right - virtualBounds.left, virtualBounds.bottom - virtualBounds.top,
        nullptr, nullptr, hInstance, &state);

    if (!hwnd) return false;

    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);

    MSG msg;
    while (!state.done && GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    outSelection = state.outSelection;
    return state.accepted;
}
