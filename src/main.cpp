#include "common.h"
#include "video.h"
#include "audio.h"
#include "renderer.h"
#include "settings.h"
#include "ui.h"
#include <mfapi.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>

enum : UINT { WM_APP_VIDEO = WM_APP + 1, WM_APP_AUDIO };

enum : UINT {
    ID_VIDEO_AUTO = 100,
    ID_MODE_AUTO,
    ID_AUDIO_AUTO,
    ID_AUDIO_OFF,
    ID_SCALE_SHARP,
    ID_SCALE_SMOOTH,
    ID_SCALE_PIXEL,
    ID_ASPECT,
    ID_TEARING,
    ID_OPEN_SCREENSHOTS,
    ID_OPEN_LOG,
    ID_EXIT,
    ID_VIDEO_BASE = 1000,
    ID_MODE_BASE = 2000,
    ID_AUDIO_BASE = 3000,
};

static const UINT_PTR kUiTimer = 1;
static const ULONGLONG kIdleMs = 2500;  // hide the bar and cursor after this long without mouse movement

static HWND g_hwnd;
static Settings g_set;
static VideoCapture g_video;
static AudioBridge g_audio;
static Renderer g_renderer;
static bool g_fullscreen = false, g_closing = false;
static WINDOWPLACEMENT g_windowedPlacement = {sizeof(WINDOWPLACEMENT)};

// Control bar / mouse state.
static UiState g_ui;
static bool g_videoRunning = false, g_menuOpen = false, g_dragging = false, g_tracking = false;
static bool g_cursorHidden = false;
static ULONGLONG g_lastMouseMove = 0;
static POINT g_lastMousePos = {-1, -1};

// ---------------------------------------------------------------- helpers

static float DpiScale() { return std::max(1.0f, GetDpiForWindow(g_hwnd) / 96.0f); }

static UiLayout CurrentLayout() {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    return LayoutBar(float(rc.right), float(rc.bottom), DpiScale());
}

static void PushOptions() {
    g_renderer.SetOptions(g_set.keepAspect, g_set.allowTearing, Scaling(g_set.scaling));
}

static void PushUi() {
    g_ui.muted = g_set.muted;
    g_ui.volume = g_set.volume;
    g_ui.fullscreen = g_fullscreen;
    g_ui.onTop = g_set.alwaysOnTop;
    g_ui.stats = g_set.showStats;
    g_renderer.SetUi(g_ui);
}

static bool CursorInClient() {
    POINT pt;
    GetCursorPos(&pt);
    if (WindowFromPoint(pt) != g_hwnd) return false;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    ScreenToClient(g_hwnd, &pt);
    return PtInRect(&rc, pt) != FALSE;
}

// The bar stays up while there's no picture, while the mouse is moving or on
// it, and while a menu is open; otherwise it (and the cursor) fade away.
static void RefreshBar() {
    const bool want = !g_videoRunning || g_menuOpen || g_dragging || g_ui.hover != UiId::None ||
                      GetTickCount64() - g_lastMouseMove < kIdleMs;
    if (want != g_ui.barVisible) {
        g_ui.barVisible = want;
        PushUi();
    }
    const bool hide = !want && CursorInClient() && GetForegroundWindow() == g_hwnd;
    if (hide != g_cursorHidden) {
        g_cursorHidden = hide;
        SetCursor(hide ? nullptr : LoadCursorW(nullptr, IDC_ARROW));
    }
}

// ---------------------------------------------------------------- actions

static void ApplyTopmost() {
    SetWindowPos(g_hwnd, g_set.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void SetFullscreen(bool on) {
    if (on == g_fullscreen) return;
    DWORD style = GetWindowLongW(g_hwnd, GWL_STYLE);
    if (on) {
        GetWindowPlacement(g_hwnd, &g_windowedPlacement);
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(g_hwnd, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(g_hwnd, GWL_STYLE, (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(g_hwnd, &g_windowedPlacement);
        SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    g_fullscreen = g_set.fullscreen = on;
    ApplyTopmost();
    PushUi();
}

static void SetVolume(float v, bool osd) {
    g_set.volume = std::clamp(std::round(v * 100.0f) / 100.0f, 0.0f, 1.0f);
    g_set.muted = false;
    g_audio.SetVolume(g_set.volume);
    g_audio.SetMute(false);
    if (osd) g_renderer.ShowOsd(L"Volume " + std::to_wstring(int(std::lround(g_set.volume * 100))) + L"%");
    PushUi();
}

static void ToggleMute() {
    g_set.muted = !g_set.muted;
    g_audio.SetMute(g_set.muted);
    g_renderer.ShowOsd(g_set.muted ? L"Muted" : L"Sound on");
    PushUi();
}

static void ToggleTopmost() {
    g_set.alwaysOnTop = !g_set.alwaysOnTop;
    ApplyTopmost();
    g_renderer.ShowOsd(g_set.alwaysOnTop ? L"Keeping CapView on top" : L"Normal window");
    PushUi();
}

static void ToggleStats() {
    g_set.showStats = !g_set.showStats;
    PushUi();
}

static void ToggleAspect() {
    g_set.keepAspect = !g_set.keepAspect;
    PushOptions();
    g_renderer.ShowOsd(g_set.keepAspect ? L"Keep aspect ratio" : L"Stretch to window");
}

// ---------------------------------------------------------------- status

static void UpdateStatus() {
    const VideoCapture::Status vs = g_video.GetStatus();
    const AudioBridge::Status as = g_audio.GetStatus();
    std::wstring title = L"CapView";
    g_videoRunning = vs.state == VideoState::Running;

    switch (vs.state) {
        case VideoState::Searching:
            g_renderer.ClearVideo();
            g_renderer.SetStatus(L"Waiting for your capture card…",
                                 L"Plug it into a USB port and it'll show up here automatically.");
            break;
        case VideoState::Opening:
        case VideoState::Running:
            g_renderer.SetStatus(L"Connecting to " + vs.device + L"…", L"");
            if (g_videoRunning) {
                title += L"  ·  " + vs.mode.Short();
                g_audio.SetVideoHint(vs.symlink);
            }
            break;
        case VideoState::Busy:
            g_renderer.ClearVideo();
            g_renderer.SetStatus(L"The capture card is busy",
                                 L"Another app (OBS, Discord, a browser tab…) is using it. Close that app and "
                                 L"CapView will connect on its own.");
            break;
        case VideoState::Blocked:
            g_renderer.ClearVideo();
            g_renderer.SetStatus(L"Windows is blocking camera access",
                                 L"Open Settings › Privacy & security › Camera and turn on “Let desktop apps access "
                                 L"your camera”.");
            break;
        case VideoState::Error:
            g_renderer.ClearVideo();
            g_renderer.SetStatus(L"Couldn't open " + vs.device,
                                 L"Retrying… If it keeps failing, try another USB port.");
            break;
    }
    SetWindowTextW(g_hwnd, title.c_str());
    // Controller input goes to the Switch, not the PC, so stop the screen from sleeping.
    SetThreadExecutionState(g_videoRunning ? (ES_CONTINUOUS | ES_DISPLAY_REQUIRED) : ES_CONTINUOUS);

    std::wstring audio = as.state == AudioState::Running ? as.input + L"  →  " + as.output
                         : as.state == AudioState::Off   ? L"off"
                         : as.state == AudioState::Error ? L"error (see log)"
                                                         : L"no audio input found";
    g_ui.statsText = g_videoRunning ? vs.device + L"\n" + vs.mode.Label() + L"  ·  " + vs.mode.Codec() +
                                          (vs.mode.Uncompressed() ? L" (uncompressed)" : L" (compressed)")
                                    : L"No video";
    g_ui.statsText += L"\nAudio: " + audio;
    PushUi();
    RefreshBar();
}

// ---------------------------------------------------------------- settings menu

static void ShowSettingsMenu(int x, int y, bool aboveAnchor) {
    const VideoCapture::Status vs = g_video.GetStatus();
    const AudioBridge::Status as = g_audio.GetStatus();
    const std::vector<DeviceInfo> devices = VideoCapture::Enumerate();
    const std::vector<VideoMode> qualities = VideoCapture::QualityChoices(vs.modes);
    const std::vector<std::wstring> inputs = AudioBridge::EnumInputs();
    auto item = [](HMENU m, UINT id, const std::wstring& text, bool checked) {
        AppendMenuW(m, MF_STRING | (checked ? MF_CHECKED : 0), id, text.c_str());
    };
    auto active = [](bool is) { return is ? std::wstring(L"   (in use)") : std::wstring(); };

    HMENU video = CreatePopupMenu();
    item(video, ID_VIDEO_AUTO, L"Automatic", g_set.videoDevice.empty());
    AppendMenuW(video, MF_SEPARATOR, 0, nullptr);
    for (size_t i = 0; i < devices.size() && i < 100; ++i)
        item(video, ID_VIDEO_BASE + UINT(i), devices[i].name + active(g_videoRunning && devices[i].id == vs.symlink),
             g_set.videoDevice == devices[i].name);

    HMENU quality = CreatePopupMenu();
    item(quality, ID_MODE_AUTO,
         g_videoRunning && g_set.modeKey.empty() ? L"Automatic  (" + vs.mode.Label() + L")" : L"Automatic",
         g_set.modeKey.empty());
    AppendMenuW(quality, MF_SEPARATOR, 0, nullptr);
    if (qualities.empty()) AppendMenuW(quality, MF_STRING | MF_GRAYED, 0, L"Connect a capture card first");
    for (size_t i = 0; i < qualities.size() && i < 100; ++i)
        item(quality, ID_MODE_BASE + UINT(i), qualities[i].Label(), g_set.modeKey == qualities[i].Key());

    HMENU audio = CreatePopupMenu();
    item(audio, ID_AUDIO_AUTO, L"Automatic", g_set.audioDevice.empty());
    item(audio, ID_AUDIO_OFF, L"Off", g_set.audioDevice == L"off");
    AppendMenuW(audio, MF_SEPARATOR, 0, nullptr);
    for (size_t i = 0; i < inputs.size() && i < 100; ++i)
        item(audio, ID_AUDIO_BASE + UINT(i), inputs[i] + active(as.state == AudioState::Running && inputs[i] == as.input),
             g_set.audioDevice == inputs[i]);

    HMENU scaling = CreatePopupMenu();
    item(scaling, ID_SCALE_SHARP, L"Sharp  (best when enlarging)", g_set.scaling == 0);
    item(scaling, ID_SCALE_SMOOTH, L"Smooth", g_set.scaling == 1);
    item(scaling, ID_SCALE_PIXEL, L"Pixel-perfect", g_set.scaling == 2);

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)video, L"Video source");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)quality, L"Quality");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)audio, L"Audio source");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)scaling, L"Scaling");
    item(menu, ID_ASPECT, L"Keep aspect ratio\tA", g_set.keepAspect);
    item(menu, ID_TEARING, L"Lowest latency (may tear)", g_set.allowTearing);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    item(menu, ID_OPEN_SCREENSHOTS, L"Open screenshots folder", false);
    item(menu, ID_OPEN_LOG, L"Open log file", false);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    item(menu, ID_EXIT, L"Exit", false);

    g_menuOpen = true;
    RefreshBar();
    const UINT flags = TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY | (aboveAnchor ? TPM_BOTTOMALIGN : 0);
    const UINT cmd = TrackPopupMenu(menu, flags, x, y, 0, g_hwnd, nullptr);
    DestroyMenu(menu);  // also destroys the submenus
    g_menuOpen = false;
    g_ui.hover = g_ui.pressed = UiId::None;
    g_lastMouseMove = GetTickCount64();
    RefreshBar();
    PushUi();

    if (cmd == ID_VIDEO_AUTO || (cmd >= ID_VIDEO_BASE && cmd < ID_VIDEO_BASE + devices.size())) {
        g_set.videoDevice = cmd == ID_VIDEO_AUTO ? L"" : devices[cmd - ID_VIDEO_BASE].name;
        g_set.modeKey.clear();
        g_video.Select(g_set.videoDevice, g_set.modeKey);
    } else if (cmd == ID_MODE_AUTO || (cmd >= ID_MODE_BASE && cmd < ID_MODE_BASE + qualities.size())) {
        g_set.modeKey = cmd == ID_MODE_AUTO ? L"" : qualities[cmd - ID_MODE_BASE].Key();
        g_video.Select(g_set.videoDevice, g_set.modeKey);
    } else if (cmd == ID_AUDIO_AUTO || cmd == ID_AUDIO_OFF ||
               (cmd >= ID_AUDIO_BASE && cmd < ID_AUDIO_BASE + inputs.size())) {
        g_set.audioDevice = cmd == ID_AUDIO_AUTO ? L"" : cmd == ID_AUDIO_OFF ? L"off" : inputs[cmd - ID_AUDIO_BASE];
        g_audio.SetInput(g_set.audioDevice);
    } else if (cmd >= ID_SCALE_SHARP && cmd <= ID_SCALE_PIXEL) {
        g_set.scaling = int(cmd - ID_SCALE_SHARP);
        PushOptions();
    } else {
        switch (cmd) {
            case ID_ASPECT: ToggleAspect(); break;
            case ID_TEARING: g_set.allowTearing = !g_set.allowTearing; PushOptions(); break;
            case ID_OPEN_SCREENSHOTS:
                ShellExecuteW(nullptr, L"open", ScreenshotDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                break;
            case ID_OPEN_LOG: ShellExecuteW(nullptr, L"open", LogPath().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
            case ID_EXIT: PostMessageW(g_hwnd, WM_CLOSE, 0, 0); break;
        }
    }
    if (cmd) g_set.Save();
}

// ---------------------------------------------------------------- mouse

static void Activate(UiId id) {
    switch (id) {
        case UiId::Mute: ToggleMute(); break;
        case UiId::Screenshot: g_renderer.RequestScreenshot(); break;
        case UiId::Stats: ToggleStats(); break;
        case UiId::Pin: ToggleTopmost(); break;
        case UiId::Fullscreen: SetFullscreen(!g_fullscreen); break;
        case UiId::Settings: {
            UiLayout l = CurrentLayout();
            for (auto& item : l.items)
                if (item.id == UiId::Settings) {
                    POINT pt = {LONG(item.rect.l), LONG(l.bar.t - 6 * DpiScale())};
                    ClientToScreen(g_hwnd, &pt);
                    ShowSettingsMenu(pt.x, pt.y, true);
                }
            break;
        }
        default: break;
    }
}

static UiId HitBar(int x, int y) {
    return g_ui.barVisible ? HitTest(CurrentLayout(), float(x), float(y)) : UiId::None;
}

static void OnMouseMove(int x, int y) {
    if (x == g_lastMousePos.x && y == g_lastMousePos.y) return;  // ignore synthetic moves
    g_lastMousePos = {x, y};
    g_lastMouseMove = GetTickCount64();
    if (!g_tracking) {
        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, g_hwnd, 0};
        g_tracking = TrackMouseEvent(&tme) != FALSE;
    }
    RefreshBar();
    if (g_dragging) {
        SetVolume(VolumeAt(CurrentLayout(), float(x)), false);
        return;
    }
    const UiId hover = HitBar(x, y);
    if (hover != g_ui.hover) {
        g_ui.hover = hover;
        PushUi();
    }
}

static void OnMouseDown(int x, int y, bool doubleClick) {
    const UiId id = HitBar(x, y);
    if (id == UiId::None) {
        if (doubleClick) SetFullscreen(!g_fullscreen);
        return;
    }
    SetCapture(g_hwnd);
    if (id == UiId::Volume) {
        g_dragging = true;
        SetVolume(VolumeAt(CurrentLayout(), float(x)), false);
    }
    g_ui.pressed = id;
    PushUi();
}

static void OnMouseUp(int x, int y) {
    const UiId pressed = g_ui.pressed;
    g_ui.pressed = UiId::None;
    g_dragging = false;
    if (GetCapture() == g_hwnd) ReleaseCapture();
    PushUi();
    if (pressed != UiId::None && pressed != UiId::Volume && HitBar(x, y) == pressed) Activate(pressed);
}

// ---------------------------------------------------------------- window

static void SaveWindowPlacement() {
    WINDOWPLACEMENT wp = {sizeof(wp)};
    if (g_fullscreen) wp = g_windowedPlacement;
    else GetWindowPlacement(g_hwnd, &wp);
    g_set.window = wp.rcNormalPosition;
    g_set.maximized = wp.showCmd == SW_SHOWMAXIMIZED;
    g_set.hasWindow = true;
}

static bool OnKey(WPARAM key) {
    switch (key) {
        case VK_F11:
        case 'F': SetFullscreen(!g_fullscreen); return true;
        case VK_ESCAPE: SetFullscreen(false); return true;
        case 'M': ToggleMute(); return true;
        case VK_UP: SetVolume(g_set.volume + 0.05f, true); return true;
        case VK_DOWN: SetVolume(g_set.volume - 0.05f, true); return true;
        case 'A': ToggleAspect(); return true;
        case 'T': ToggleTopmost(); return true;
        case 'S': g_renderer.RequestScreenshot(); return true;
        case 'I': ToggleStats(); return true;
    }
    return false;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            if (LOWORD(lp) && HIWORD(lp)) g_renderer.Resize();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {360, 240};
            return 0;
        case WM_DPICHANGED: {
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_APP_VIDEO:
        case WM_APP_AUDIO:
            UpdateStatus();
            return 0;
        case WM_MOUSEMOVE:
            OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        case WM_MOUSELEAVE:
            g_tracking = false;
            g_lastMousePos = {-1, -1};
            g_lastMouseMove = GetTickCount64() - kIdleMs + 500;  // fade out shortly after leaving
            if (g_ui.hover != UiId::None) {
                g_ui.hover = UiId::None;
                PushUi();
            }
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            OnMouseDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), msg == WM_LBUTTONDBLCLK);
            return 0;
        case WM_LBUTTONUP:
            OnMouseUp(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        case WM_CAPTURECHANGED:
            g_dragging = false;
            return 0;
        case WM_CONTEXTMENU: {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (pt.x == -1 && pt.y == -1) GetCursorPos(&pt);  // keyboard menu key
            ShowSettingsMenu(pt.x, pt.y, false);
            return 0;
        }
        case WM_MOUSEWHEEL:
            SetVolume(g_set.volume + (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 0.05f : -0.05f), true);
            return 0;
        case WM_TIMER:
            if (wp == kUiTimer) RefreshBar();
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                SetCursor(g_cursorHidden ? nullptr : LoadCursorW(nullptr, IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_KEYDOWN:
            if (OnKey(wp)) return 0;
            break;
        case WM_SYSKEYDOWN:
            if (wp == VK_RETURN && (HIWORD(lp) & KF_ALTDOWN)) {
                SetFullscreen(!g_fullscreen);
                return 0;
            }
            break;
        case WM_SYSCHAR:
            if (wp == VK_RETURN) return 0;  // no ding on Alt+Enter
            break;
        case WM_CLOSE:
            if (g_closing) return 0;
            g_closing = true;
            SaveWindowPlacement();
            g_set.Save();
            ShowWindow(hwnd, SW_HIDE);
            g_video.Stop();
            g_audio.Stop();
            g_renderer.Shutdown();
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int showCmd) {
    // Only one CapView can own the card; a second launch just focuses the first.
    CreateMutexW(nullptr, TRUE, L"CapView.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(L"CapViewWindow", nullptr)) {
            if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        return 0;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    MFStartup(MF_VERSION);
    LogInit();
    g_set.Load();

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"CapViewWindow";
    RegisterClassExW(&wc);

    // Default: a 1280x720 video area centered on the primary monitor.
    RECT rc = {0, 0, 1280, 720};
    AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0, GetDpiForSystem());
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"CapView", WS_OVERLAPPEDWINDOW,
                             (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h,
                             nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) return 1;

    // Dark title bar everywhere; on Windows 11 also tint it near-black so it
    // blends into the video (older Windows ignores the color attributes).
    BOOL dark = TRUE;
    COLORREF caption = RGB(14, 14, 16), text = RGB(170, 170, 176);
    DwmSetWindowAttribute(g_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    DwmSetWindowAttribute(g_hwnd, 34 /* DWMWA_BORDER_COLOR */, &caption, sizeof(caption));
    DwmSetWindowAttribute(g_hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
    DwmSetWindowAttribute(g_hwnd, 36 /* DWMWA_TEXT_COLOR */, &text, sizeof(text));
    if (g_set.hasWindow && MonitorFromRect(&g_set.window, MONITOR_DEFAULTTONULL)) {
        WINDOWPLACEMENT wp = {sizeof(wp)};
        wp.rcNormalPosition = g_set.window;
        wp.showCmd = SW_HIDE;
        SetWindowPlacement(g_hwnd, &wp);
    }

    if (!g_renderer.Init(g_hwnd)) {
        MessageBoxW(g_hwnd, L"CapView couldn't start Direct3D on this PC.", L"CapView", MB_ICONERROR);
        return 1;
    }
    PushOptions();
    g_renderer.SetStatus(L"Looking for your capture card…", L"");

    g_audio.SetVolume(g_set.volume);
    g_audio.SetMute(g_set.muted);
    g_audio.SetInput(g_set.audioDevice);
    g_audio.Start(g_hwnd, WM_APP_AUDIO);
    g_video.Select(g_set.videoDevice, g_set.modeKey);
    g_video.Start([](Frame&& f) { g_renderer.SubmitFrame(std::move(f)); }, g_hwnd, WM_APP_VIDEO);

    ShowWindow(g_hwnd, g_set.maximized ? SW_SHOWMAXIMIZED : showCmd);
    if (g_set.alwaysOnTop) ApplyTopmost();
    if (g_set.fullscreen) SetFullscreen(true);
    g_lastMouseMove = GetTickCount64();  // show the bar for a moment at startup
    SetTimer(g_hwnd, kUiTimer, 200, nullptr);
    UpdateStatus();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    SetThreadExecutionState(ES_CONTINUOUS);
    MFShutdown();
    CoUninitialize();
    return 0;
}
