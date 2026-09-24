#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shellscalingapi.h>
#include <string>
#include <optional>
#include <algorithm>
#include "Capture.h"
#include "Overlay.h"
#include "ImageIO.h"
#include "Settings.h"
#include "Startup.h"
#include "resource.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "Shcore.lib")

namespace
{
    constexpr UINT WM_TRAYICON = WM_APP + 1;
    constexpr int ID_HOTKEY_CAPTURE = 1;
    constexpr int ID_HOTKEY_REPEAT = 2;

    constexpr UINT_PTR ID_MENU_CAPTURE = 1001;
    constexpr UINT_PTR ID_MENU_REPEAT = 1002;
    constexpr UINT_PTR ID_MENU_OPEN_FOLDER = 1003;
    constexpr UINT_PTR ID_MENU_CHANGE_FOLDER = 1004;
    constexpr UINT_PTR ID_MENU_STARTUP = 1005;
    constexpr UINT_PTR ID_MENU_EXIT = 1006;
    constexpr UINT_PTR ID_MENU_DIAGNOSTIC = 1007;

    HINSTANCE g_hInstance = nullptr;
    HWND g_hwnd = nullptr;
    NOTIFYICONDATA g_nid{};
    AppSettings g_settings;
    std::optional<RECT> g_lastRegion;
    std::wstring g_lastSavedFilePath;

    bool FileExists(const std::wstring& path)
    {
        DWORD attrs = GetFileAttributesW(path.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
    }

    // Ouvre l'explorateur avec le dernier fichier capturé présélectionné (surligné) plutôt que
    // de simplement ouvrir le dossier. Se rabat sur l'ouverture simple du dossier si aucun
    // fichier n'a encore été produit ou s'il a été supprimé depuis.
    void RevealLastCaptureInExplorer()
    {
        if (!g_lastSavedFilePath.empty() && FileExists(g_lastSavedFilePath))
        {
            std::wstring params = L"/select,\"" + g_lastSavedFilePath + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
        }
        else
        {
            CreateDirectoryW(g_settings.saveFolder.c_str(), nullptr);
            ShellExecuteW(nullptr, L"open", g_settings.saveFolder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }

    std::wstring MakeTimestampedFilename()
    {
        SYSTEMTIME st; GetLocalTime(&st);
        wchar_t buf[64];
        swprintf_s(buf, L"%04d-%02d-%02d_%02d-%02d-%02d.png",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        return buf;
    }

    void ShowBalloon(const std::wstring& title, const std::wstring& text)
    {
        NOTIFYICONDATA nid = g_nid;
        nid.uFlags = NIF_INFO;
        wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
        wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
        nid.dwInfoFlags = NIIF_INFO;
        Shell_NotifyIcon(NIM_MODIFY, &nid);
    }

    void SaveAndCopy(const CapturedImage& full, RECT cropLocal)
    {
        CreateDirectoryW(g_settings.saveFolder.c_str(), nullptr);
        std::wstring filename = MakeTimestampedFilename();
        std::wstring path = g_settings.saveFolder + L"\\" + filename;

        bool savedOk = SaveCroppedPng(full, cropLocal, path);
        CopyCroppedToClipboard(g_hwnd, full, cropLocal);

        if (savedOk)
        {
            g_lastSavedFilePath = path;
            ShowBalloon(L"ScreenshotCropAndStore", L"Capture enregistrée : " + filename);
        }
        else
            ShowBalloon(L"ScreenshotCropAndStore", L"Échec de l'enregistrement du fichier PNG.");
    }

    void DoInteractiveCapture()
    {
        RECT bounds{};
        CapturedImage capture = CaptureVirtualScreen(bounds);
        if (!capture.IsValid()) return;

        RECT selection{};
        bool accepted = ShowSelectionOverlay(g_hInstance, capture, bounds, selection);
        if (!accepted) return;

        g_lastRegion = selection;
        SaveAndCopy(capture, selection);
    }

    void DoRepeatCapture()
    {
        if (!g_lastRegion.has_value())
        {
            // Pas de zone précédente en mémoire : on retombe sur une capture classique.
            DoInteractiveCapture();
            return;
        }

        RECT bounds{};
        CapturedImage capture = CaptureVirtualScreen(bounds);
        if (!capture.IsValid()) return;

        RECT region = *g_lastRegion;
        RECT safeRegion;
        safeRegion.left = (std::max)(0L, region.left);
        safeRegion.top = (std::max)(0L, region.top);
        safeRegion.right = (std::min)(static_cast<LONG>(capture.width), region.right);
        safeRegion.bottom = (std::min)(static_cast<LONG>(capture.height), region.bottom);

        if (safeRegion.right <= safeRegion.left || safeRegion.bottom <= safeRegion.top)
        {
            ShowBalloon(L"ScreenshotCropAndStore", L"La zone précédente n'est plus valide (résolution/écrans modifiés).");
            return;
        }

        SaveAndCopy(capture, safeRegion);
    }

    void ChangeFolderDialog(HWND owner)
    {
        IFileOpenDialog* dialog = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog))))
            return;

        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS);

        if (SUCCEEDED(dialog->Show(owner)))
        {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item)))
            {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                {
                    g_settings.saveFolder = path;
                    SettingsManager::Save(g_settings);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }

    void ShowDpiDiagnostics(HWND hwnd)
    {
        DPI_AWARENESS_CONTEXT ctx = GetThreadDpiAwarenessContext();
        BOOL isPMv2 = AreDpiAwarenessContextsEqual(ctx, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

        RECT bounds{};
        CapturedImage capture = CaptureVirtualScreen(bounds);

        wchar_t buf[1024];
        swprintf_s(buf,
            L"GetSystemMetrics (SM_CXVIRTUALSCREEN x SM_CYVIRTUALSCREEN) : %d x %d\n"
            L"Origine bureau virtuel (SM_XVIRTUALSCREEN, SM_YVIRTUALSCREEN) : %d, %d\n\n"
            L"Image capturée (BitBlt) : %d x %d\n\n"
            L"GetDpiForSystem() : %u (96 = 100%%, 144 = 150%%)\n"
            L"GetDpiForWindow(fenêtre de message) : %u\n\n"
            L"Contexte DPI du thread == Per-Monitor V2 ? %s\n\n"
            L"Résolution d'écran réelle attendue (Windows, Paramètres d'affichage) : compare avec\n"
            L"la ligne \"GetSystemMetrics\" ci-dessus. Si GetSystemMetrics est plus PETIT que ta\n"
            L"vraie résolution physique, l'awareness DPI par moniteur n'est PAS active.",
            GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN),
            GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
            capture.width, capture.height,
            GetDpiForSystem(),
            GetDpiForWindow(hwnd),
            isPMv2 ? L"OUI" : L"NON");

        MessageBoxW(hwnd, buf, L"Diagnostic DPI — ScreenshotCropAndStore", MB_OK | MB_ICONINFORMATION);
    }

    void ShowTrayMenu(HWND hwnd)
    {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, ID_MENU_CAPTURE, L"Nouvelle capture");
        AppendMenuW(menu, MF_STRING, ID_MENU_REPEAT, L"Répéter la dernière zone (Ctrl+Impr écran)");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ID_MENU_OPEN_FOLDER, L"Ouvrir le dossier (dernier fichier sélectionné)");
        AppendMenuW(menu, MF_STRING, ID_MENU_CHANGE_FOLDER, L"Changer le dossier de sauvegarde...");
        AppendMenuW(menu, MF_STRING | (StartupHelper::IsEnabled() ? MF_CHECKED : 0),
            ID_MENU_STARTUP, L"Démarrer avec Windows");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ID_MENU_DIAGNOSTIC, L"Diagnostic DPI");
        AppendMenuW(menu, MF_STRING, ID_MENU_EXIT, L"Quitter");

        POINT pt; GetCursorPos(&pt);
        SetForegroundWindow(hwnd); // nécessaire pour que le menu se referme correctement
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
        PostMessage(hwnd, WM_NULL, 0, 0); // astuce classique pour fiabiliser la fermeture du menu
        DestroyMenu(menu);
    }

    LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_HOTKEY:
            if (wParam == ID_HOTKEY_CAPTURE) DoInteractiveCapture();
            else if (wParam == ID_HOTKEY_REPEAT) DoRepeatCapture();
            return 0;

        case WM_TRAYICON:
            if (LOWORD(lParam) == WM_LBUTTONDBLCLK) DoInteractiveCapture();
            else if (LOWORD(lParam) == WM_RBUTTONUP) ShowTrayMenu(hwnd);
            // Clic sur la bulle de notification elle-même (pas sur l'icône) : ouvre
            // l'explorateur avec le fichier tout juste capturé présélectionné.
            else if (LOWORD(lParam) == NIN_BALLOONUSERCLICK) RevealLastCaptureInExplorer();
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
            case ID_MENU_CAPTURE: DoInteractiveCapture(); break;
            case ID_MENU_REPEAT: DoRepeatCapture(); break;
            case ID_MENU_OPEN_FOLDER:
                RevealLastCaptureInExplorer();
                break;
            case ID_MENU_CHANGE_FOLDER:
                ChangeFolderDialog(hwnd);
                break;
            case ID_MENU_STARTUP:
            {
                bool newState = !StartupHelper::IsEnabled();
                StartupHelper::SetEnabled(newState);
                g_settings.startWithWindows = newState;
                SettingsManager::Save(g_settings);
                break;
            }
            case ID_MENU_DIAGNOSTIC:
                ShowDpiDiagnostics(hwnd);
                break;
            case ID_MENU_EXIT:
                DestroyWindow(hwnd);
                break;
            }
            return 0;

        case WM_DESTROY:
            Shell_NotifyIcon(NIM_DELETE, &g_nid);
            UnregisterHotKey(hwnd, ID_HOTKEY_CAPTURE);
            UnregisterHotKey(hwnd, ID_HOTKEY_REPEAT);
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
    // Défense en profondeur : empêche le chargement de DLL depuis des dossiers non fiables
    // (ex. un partage réseau ou un dossier Téléchargements) si une dépendance dynamique venait
    // à être ajoutée plus tard. Nos dépendances actuelles (d2d1, dwrite) sont des "KnownDLLs"
    // système déjà protégées par Windows, mais ce réglage est gratuit et sans effet de bord.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR);

    // Filet de sécurité : force l'awareness DPI par moniteur le plus tôt possible, avant toute création de fenêtre
    // ou tout appel dépendant du DPI (le manifeste ne déclare plus rien à ce sujet, donc rien
    // ne peut bloquer cet appel). Repli sur l'API Windows 8.1 si la méthode 2016 échoue.
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
    {
        SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"ScreenshotCropAndStore_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        MessageBoxW(nullptr, L"ScreenshotCropAndStore est déjà lancé (regarde la zone de notification).",
            L"ScreenshotCropAndStore", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    g_hInstance = hInstance;
    g_settings = SettingsManager::Load();
    CreateDirectoryW(g_settings.saveFolder.c_str(), nullptr);

    const wchar_t* className = L"ScreenshotCropAndStoreMainWndClass";
    WNDCLASS wc{};
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = className;
    RegisterClass(&wc);

    // Fenêtre "message-only" : invisible, sert uniquement à recevoir les messages
    // (raccourcis clavier, callback de l'icône de tray).
    g_hwnd = CreateWindowEx(0, className, L"ScreenshotCropAndStore", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, hInstance, nullptr);

    bool okCapture = RegisterHotKey(g_hwnd, ID_HOTKEY_CAPTURE, 0, VK_SNAPSHOT);
    bool okRepeat = RegisterHotKey(g_hwnd, ID_HOTKEY_REPEAT, MOD_CONTROL, VK_SNAPSHOT);
    if (!okCapture || !okRepeat)
    {
        MessageBoxW(nullptr,
            L"Impossible d'enregistrer un ou plusieurs raccourcis (Impr écran / Ctrl+Impr écran).\n"
            L"Une autre application les utilise peut-être déjà.\n\n"
            L"Tu peux toujours déclencher une capture depuis l'icône dans la barre des tâches.",
            L"ScreenshotCropAndStore", MB_OK | MB_ICONWARNING);
    }

    g_nid.cbSize = sizeof(NOTIFYICONDATA);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APP_ICON));
    wcsncpy_s(g_nid.szTip, L"ScreenshotCropAndStore — Impr écran pour capturer", _TRUNCATE);
    Shell_NotifyIcon(NIM_ADD, &g_nid);

    if (g_settings.startWithWindows && !StartupHelper::IsEnabled())
    {
        StartupHelper::SetEnabled(true);
    }

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    CoUninitialize();
    CloseHandle(mutex);
    return static_cast<int>(msg.wParam);
}
