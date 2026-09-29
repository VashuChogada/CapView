#pragma once
#include "common.h"
#include <mfidl.h>
#include <mfreadwrite.h>
#include <thread>
#include <mutex>
#include <atomic>

struct Frame {
    ComPtr<IMFSample> sample;
    UINT32 width = 0, height = 0;
    LONG stride = 0;
    PixFmt fmt = PixFmt::None;
    UINT32 nominalRange = 0;  // MFNominalRange, 0 = unknown
    UINT32 yuvMatrix = 0;     // MFVideoTransferMatrix, 0 = unknown
    bool mjpegSource = false;
};

enum class VideoState { Searching, Opening, Running, Busy, Blocked, Error };

// Owns a capture thread that finds the capture card, opens it with Media
// Foundation and hands every decoded frame to a callback. Reconnects on its own
// when the card is unplugged / replugged.
class VideoCapture {
public:
    using FrameFn = std::function<void(Frame&&)>;
    struct Status {
        VideoState state = VideoState::Searching;
        std::wstring device, symlink;
        VideoMode mode;
        std::vector<VideoMode> modes;
        HRESULT lastError = S_OK;
    };

    void Start(FrameFn onFrame, HWND notifyWnd, UINT notifyMsg);
    void Stop();
    // L"" means automatic for both.
    void Select(const std::wstring& device, const std::wstring& modeKey);
    Status GetStatus();
    static std::vector<DeviceInfo> Enumerate();
    // The short list shown in the Quality menu: one entry per resolution/fps.
    static std::vector<VideoMode> QualityChoices(const std::vector<VideoMode>& modes);

private:
    void Run();
    HRESULT OpenAndStream(const DeviceInfo& dev, const std::wstring& modeKey);
    HRESULT StreamFrom(IMFMediaSource* source, const DeviceInfo& dev, const std::wstring& modeKey);
    void Publish(VideoState s, const DeviceInfo* dev = nullptr, HRESULT hr = S_OK);
    void InterruptSource();

    FrameFn onFrame_;
    HWND notifyWnd_ = nullptr;
    UINT notifyMsg_ = 0;
    std::thread thread_;
    HANDLE wake_ = nullptr;
    std::atomic<bool> stop_{false}, restart_{false};
    std::mutex mutex_;
    std::wstring reqDevice_, reqMode_;
    Status status_;
    ComPtr<IMFMediaSource> source_;
    std::vector<std::wstring> tooSlow_;  // mode Ids that underdelivered this session (capture thread only)
};
