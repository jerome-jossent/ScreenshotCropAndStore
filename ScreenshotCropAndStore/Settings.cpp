#include "Settings.h"
#include <windows.h>
#include <shlobj.h>
#include <fstream>

#pragma comment(lib, "ole32.lib")

namespace
{
    std::wstring GetSettingsFolder()
    {
        PWSTR appData = nullptr;
        std::wstring folder;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)))
        {
            folder = std::wstring(appData) + L"\\ScreenshotCropAndStore";
            CoTaskMemFree(appData);
        }
        return folder;
    }

    std::wstring GetSettingsFile()
    {
        return GetSettingsFolder() + L"\\settings.ini";
    }

    std::wstring DefaultSaveFolder()
    {
        PWSTR pictures = nullptr;
        std::wstring folder;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pictures)))
        {
            folder = std::wstring(pictures) + L"\\Captures";
            CoTaskMemFree(pictures);
        }
        return folder;
    }
}

AppSettings SettingsManager::Load()
{
    AppSettings settings;
    settings.saveFolder = DefaultSaveFolder();
    settings.startWithWindows = true;

    std::wifstream file(GetSettingsFile());
    if (file)
    {
        std::wstring line;
        while (std::getline(file, line))
        {
            size_t eq = line.find(L'=');
            if (eq == std::wstring::npos) continue;
            std::wstring key = line.substr(0, eq);
            std::wstring value = line.substr(eq + 1);
            // Retire un éventuel retour chariot résiduel (fichier écrit/relu entre systèmes).
            while (!value.empty() && (value.back() == L'\r' || value.back() == L'\n'))
                value.pop_back();

            if (key == L"SaveFolder" && !value.empty()) settings.saveFolder = value;
            else if (key == L"StartWithWindows") settings.startWithWindows = (value == L"1");
        }
    }
    else
    {
        Save(settings);
    }
    return settings;
}

void SettingsManager::Save(const AppSettings& settings)
{
    CreateDirectoryW(GetSettingsFolder().c_str(), nullptr);
    std::wofstream file(GetSettingsFile(), std::ios::trunc);
    if (!file) return;

    file << L"SaveFolder=" << settings.saveFolder << L"\n";
    file << L"StartWithWindows=" << (settings.startWithWindows ? L"1" : L"0") << L"\n";
}
