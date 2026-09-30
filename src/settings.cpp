#include "settings.h"
#include <algorithm>
#include <cstdio>

static std::wstring IniPath() { return AppDataDir() + L"\\settings.ini"; }

static std::wstring ReadStr(const std::wstring& ini, const wchar_t* key, const wchar_t* def = L"") {
    wchar_t buf[512];
    GetPrivateProfileStringW(L"CapView", key, def, buf, 512, ini.c_str());
    return buf;
}

static int ReadInt(const std::wstring& ini, const wchar_t* key, int def) {
    return (int)GetPrivateProfileIntW(L"CapView", key, def, ini.c_str());
}

static void WriteStr(const std::wstring& ini, const wchar_t* key, const std::wstring& v) {
    WritePrivateProfileStringW(L"CapView", key, v.c_str(), ini.c_str());
}

static void WriteInt(const std::wstring& ini, const wchar_t* key, int v) {
    WriteStr(ini, key, std::to_wstring(v));
}

void Settings::Load() {
    const std::wstring ini = IniPath();
    videoDevice = ReadStr(ini, L"VideoDevice");
    modeKey = ReadStr(ini, L"VideoMode");
    audioDevice = ReadStr(ini, L"AudioDevice");
    volume = std::clamp(ReadInt(ini, L"VolumePercent", 100), 0, 200) / 100.0f;
    muted = ReadInt(ini, L"Muted", 0) != 0;
    keepAspect = ReadInt(ini, L"KeepAspect", 1) != 0;
    alwaysOnTop = ReadInt(ini, L"AlwaysOnTop", 0) != 0;
    allowTearing = ReadInt(ini, L"AllowTearing", 0) != 0;
    fullscreen = ReadInt(ini, L"Fullscreen", 0) != 0;
    showStats = ReadInt(ini, L"ShowStats", 0) != 0;
    scaling = std::clamp(ReadInt(ini, L"Scaling", 0), 0, 2);
    std::wstring rect = ReadStr(ini, L"Window");
    hasWindow = swscanf_s(rect.c_str(), L"%ld,%ld,%ld,%ld", &window.left, &window.top, &window.right,
                          &window.bottom) == 4 &&
                window.right - window.left > 100 && window.bottom - window.top > 100;
    maximized = ReadInt(ini, L"Maximized", 0) != 0;
}

void Settings::Save() const {
    const std::wstring ini = IniPath();
    WriteStr(ini, L"VideoDevice", videoDevice);
    WriteStr(ini, L"VideoMode", modeKey);
    WriteStr(ini, L"AudioDevice", audioDevice);
    WriteInt(ini, L"VolumePercent", int(volume * 100 + 0.5f));
    WriteInt(ini, L"Muted", muted);
    WriteInt(ini, L"KeepAspect", keepAspect);
    WriteInt(ini, L"AlwaysOnTop", alwaysOnTop);
    WriteInt(ini, L"AllowTearing", allowTearing);
    WriteInt(ini, L"Fullscreen", fullscreen);
    WriteInt(ini, L"ShowStats", showStats);
    WriteInt(ini, L"Scaling", scaling);
    if (hasWindow) {
        wchar_t buf[96];
        swprintf_s(buf, L"%ld,%ld,%ld,%ld", window.left, window.top, window.right, window.bottom);
        WriteStr(ini, L"Window", buf);
        WriteInt(ini, L"Maximized", maximized);
    }
}
