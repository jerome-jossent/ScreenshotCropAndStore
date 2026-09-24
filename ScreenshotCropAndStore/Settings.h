#pragma once
#include <string>

struct AppSettings
{
    std::wstring saveFolder;
    bool startWithWindows = true;
};

namespace SettingsManager
{
    AppSettings Load();
    void Save(const AppSettings& settings);
}
