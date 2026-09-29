#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <functional>

using Microsoft::WRL::ComPtr;

#include "version.h"
#define CAPVIEW_VERSION L"" CAPVIEW_VERSION_STR

// Log file: %LOCALAPPDATA%\CapView\capview.log (overwritten each launch).
void LogInit();
void Log(const wchar_t* fmt, ...);
std::wstring LogPath();

// %APPDATA%\CapView and Pictures\CapView, created on demand.
std::wstring AppDataDir();
std::wstring ScreenshotDir();

std::wstring Lower(std::wstring s);

// How likely a device name is an HDMI capture card rather than a webcam/mic.
// > 0 means "probably a capture card", < 0 means "definitely not".
int CaptureNameScore(const std::wstring& name);

// Container ID of the physical device behind a device interface path
// (e.g. a Media Foundation symbolic link). The video and audio halves of a
// capture card share one container ID, which is how we pair them.
bool GetContainerId(const std::wstring& interfacePath, GUID& out);

enum class PixFmt { None, YUY2, NV12, RGB32 };
const wchar_t* PixFmtName(PixFmt f);

struct VideoMode {
    UINT32 width = 0, height = 0, fpsNum = 0, fpsDen = 1;
    GUID subtype{};
    double Fps() const { return fpsDen ? double(fpsNum) / fpsDen : 0.0; }
    bool Uncompressed() const;   // raw YUY2/NV12: no decoding needed
    std::wstring Codec() const;  // "MJPG", "NV12", ...
    std::wstring Label() const;  // "1920×1080 · 60 fps"
    std::wstring Short() const;  // "1080p60"
    std::wstring Key() const;    // "1920x1080@60/1" (what the user picks; codec is chosen for them)
    std::wstring Id() const;     // Key + codec: one exact native mode
};

struct DeviceInfo {
    std::wstring name, id;
};
