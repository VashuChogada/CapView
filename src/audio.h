#pragma once
#include "common.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <thread>
#include <mutex>
#include <atomic>

enum class AudioState { Off, NoInput, Running, Error };

// Pipes the capture card's audio input straight to the default speakers with
// WASAPI, keeping only a few milliseconds buffered.
class AudioBridge {
public:
    struct Status {
        AudioState state = AudioState::NoInput;
        std::wstring input, output;
    };

    void Start(HWND notifyWnd, UINT notifyMsg);
    void Stop();
    void SetInput(const std::wstring& name);        // L"" = automatic, L"off" = disabled
    void SetVideoHint(const std::wstring& symlink);  // lets "automatic" pair with the video device
    void SetVolume(float v);
    void SetMute(bool m);
    Status GetStatus();
    static std::vector<std::wstring> EnumInputs();

    void Wake(bool restart);  // called from the device notifier

private:
    void Run();
    HRESULT Stream(IMMDevice* in, IMMDevice* out);
    void Publish(AudioState s, const std::wstring& in = L"", const std::wstring& out = L"");

    HWND notifyWnd_ = nullptr;
    UINT notifyMsg_ = 0;
    std::thread thread_;
    HANDLE wake_ = nullptr;
    std::atomic<bool> stop_{false}, restart_{false};
    std::atomic<float> volume_{1.0f};
    std::atomic<bool> muted_{false}, volumeDirty_{true};
    std::mutex mutex_;
    std::wstring input_, hint_;
    Status status_;
};
