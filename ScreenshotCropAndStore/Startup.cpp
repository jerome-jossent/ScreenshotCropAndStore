#include "Startup.h"
#include <windows.h>
#include <string>

namespace
{
    const wchar_t* RunKeyPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    const wchar_t* AppName = L"ScreenshotCropAndStore";
}

void StartupHelper::SetEnabled(bool enabled)
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RunKeyPath, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return;

    if (enabled)
    {
        wchar_t exePath[MAX_PATH];
        DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (len == 0 || len == MAX_PATH)
        {
            // Chemin introuvable ou tronqué (exe à plus de MAX_PATH caractères de profondeur) :
            // on n'écrit rien plutôt qu'un chemin de démarrage incomplet/faux.
            RegCloseKey(key);
            return;
        }
        std::wstring quoted = L"\"" + std::wstring(exePath) + L"\"";
        RegSetValueExW(key, AppName, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(quoted.c_str()),
            static_cast<DWORD>((quoted.size() + 1) * sizeof(wchar_t)));
    }
    else
    {
        RegDeleteValueW(key, AppName);
    }

    RegCloseKey(key);
}

bool StartupHelper::IsEnabled()
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RunKeyPath, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;

    DWORD type = 0, size = 0;
    LONG result = RegQueryValueExW(key, AppName, nullptr, &type, nullptr, &size);
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}
