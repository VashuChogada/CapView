#include "common.h"
#include <shlobj.h>
#include <mfapi.h>
#include <cfgmgr32.h>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <cwctype>
#include <algorithm>
#include <mutex>

static HANDLE g_log = INVALID_HANDLE_VALUE;
static std::mutex g_logMutex;
static std::wstring g_logPath;

static std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p))) s = p;
    CoTaskMemFree(p);
    return s;
}

std::wstring AppDataDir() {
    std::wstring dir = KnownFolder(FOLDERID_RoamingAppData) + L"\\CapView";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring ScreenshotDir() {
    std::wstring dir = KnownFolder(FOLDERID_Pictures) + L"\\CapView";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring LogPath() { return g_logPath; }

const wchar_t* PixFmtName(PixFmt f) {
    switch (f) {
        case PixFmt::YUY2: return L"YUY2";
        case PixFmt::NV12: return L"NV12";
        case PixFmt::RGB32: return L"RGB32";
        default: return L"?";
    }
}

void LogInit() {
    std::wstring dir = KnownFolder(FOLDERID_LocalAppData) + L"\\CapView";
    CreateDirectoryW(dir.c_str(), nullptr);
    g_logPath = dir + L"\\capview.log";
    g_log = CreateFileW(g_logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    Log(L"CapView %s starting", CAPVIEW_VERSION);
}

void Log(const wchar_t* fmt, ...) {
    wchar_t msg[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(msg, _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t line[1200];
    swprintf_s(line, L"%02u:%02u:%02u.%03u  %s\r\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
    OutputDebugStringW(line);

    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_log == INVALID_HANDLE_VALUE) return;
    int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return;
    std::string utf8(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8.data(), n, nullptr, nullptr);
    DWORD written = 0;
    WriteFile(g_log, utf8.data(), DWORD(n - 1), &written, nullptr);
}

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
    return s;
}

int CaptureNameScore(const std::wstring& name) {
    const std::wstring n = Lower(name);
    static const wchar_t* kNot[] = {
        L"webcam", L"web cam", L"facetime", L"integrated", L"hd camera", L"ir camera", L"virtual",
        L"obs", L"broadcast", L"droidcam", L"iriun", L"snap camera", L"c920", L"c922", L"c930",
        L"brio", L"kiyo", L"microphone", L"headset", L"steam streaming",
    };
    static const struct { const wchar_t* s; int w; } kLikely[] = {
        {L"capture", 80}, {L"hdmi", 80}, {L"cam link", 80}, {L"usb video", 60}, {L"usb3", 40},
        {L"uvc", 30}, {L"game", 40}, {L"ms2109", 90}, {L"ms2130", 90}, {L"macrosilicon", 90},
        {L"usb digital audio", 60}, {L"digital audio interface", 40}, {L"hd60", 90},
        {L"avermedia", 60}, {L"live gamer", 60},
    };
    int score = 0;
    for (auto s : kNot)
        if (n.find(s) != std::wstring::npos) score -= 100;
    for (auto& k : kLikely)
        if (n.find(k.s) != std::wstring::npos) score += k.w;
    return score;
}

static const DEVPROPKEY kDevInstanceId = {
    {0x78c34fc8, 0x104a, 0x4aca, {0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0x57}}, 256};
static const DEVPROPKEY kDevContainerId = {
    {0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};

bool GetContainerId(const std::wstring& interfacePath, GUID& out) {
    WCHAR instance[512] = {};
    ULONG size = sizeof(instance);
    DEVPROPTYPE type = 0;
    if (CM_Get_Device_Interface_PropertyW(interfacePath.c_str(), &kDevInstanceId, &type, (PBYTE)instance, &size,
                                          0) != CR_SUCCESS)
        return false;
    DEVINST devInst = 0;
    if (CM_Locate_DevNodeW(&devInst, instance, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return false;
    size = sizeof(GUID);
    return CM_Get_DevNode_PropertyW(devInst, &kDevContainerId, &type, (PBYTE)&out, &size, 0) == CR_SUCCESS;
}

std::wstring VideoMode::Codec() const {
    if (subtype == MFVideoFormat_RGB32) return L"RGB32";
    if (subtype == MFVideoFormat_RGB24) return L"RGB24";
    wchar_t s[5] = {};
    for (int i = 0; i < 4; ++i) {
        wchar_t c = wchar_t((subtype.Data1 >> (8 * i)) & 0xff);
        s[i] = (c >= 32 && c < 127) ? c : L'?';
    }
    return s;
}

bool VideoMode::Uncompressed() const { return subtype == MFVideoFormat_NV12 || subtype == MFVideoFormat_YUY2; }

std::wstring VideoMode::Label() const {
    wchar_t buf[96];
    double fps = Fps();
    if (std::abs(fps - std::round(fps)) < 0.05) swprintf_s(buf, L"%u×%u  ·  %d fps", width, height, int(std::round(fps)));
    else swprintf_s(buf, L"%u×%u  ·  %.2f fps", width, height, fps);
    return buf;
}

std::wstring VideoMode::Short() const {
    const int fps = int(std::round(Fps()));
    if (std::abs(double(width) / height - 16.0 / 9.0) < 0.03)
        return std::to_wstring(height) + L"p" + std::to_wstring(fps);
    return std::to_wstring(width) + L"×" + std::to_wstring(height) + L" " + std::to_wstring(fps) + L"fps";
}

std::wstring VideoMode::Key() const {
    wchar_t buf[64];
    swprintf_s(buf, L"%ux%u@%u/%u", width, height, fpsNum, fpsDen);
    return buf;
}

std::wstring VideoMode::Id() const { return Key() + L":" + Codec(); }
