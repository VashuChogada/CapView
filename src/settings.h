#pragma once
#include "common.h"

// Persisted in %APPDATA%\CapView\settings.ini
struct Settings {
    std::wstring videoDevice;  // L"" = automatic
    std::wstring modeKey;      // L"" = automatic, else VideoMode::Key ("1920x1080@60/1")
    std::wstring audioDevice;  // L"" = automatic, L"off" = no audio
    float volume = 1.0f;
    bool muted = false;
    bool keepAspect = true;
    bool alwaysOnTop = false;
    bool allowTearing = false;
    bool fullscreen = false;
    bool showStats = false;
    int scaling = 0;  // Scaling: 0 sharp, 1 smooth, 2 pixel
    bool hasWindow = false;
    RECT window{};
    bool maximized = false;

    void Load();
    void Save() const;
};
