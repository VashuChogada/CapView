#include "video.h"
#include <mfapi.h>
#include <mferror.h>
#include <avrt.h>
#include <algorithm>

std::vector<DeviceInfo> VideoCapture::Enumerate() {
    std::vector<DeviceInfo> out;
    ComPtr<IMFAttributes> attr;
    if (FAILED(MFCreateAttributes(&attr, 1))) return out;
    attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(attr.Get(), &devices, &count))) return out;
    for (UINT32 i = 0; i < count; ++i) {
        WCHAR* name = nullptr;
        WCHAR* link = nullptr;
        UINT32 len = 0;
        if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len)) &&
            SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link,
                                                     &len)))
            out.push_back({name, link});
        CoTaskMemFree(name);
        CoTaskMemFree(link);
        devices[i]->Release();
    }
    CoTaskMemFree(devices);
    return out;
}

static VideoMode ModeFromType(IMFMediaType* t) {
    VideoMode m;
    t->GetGUID(MF_MT_SUBTYPE, &m.subtype);
    MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &m.width, &m.height);
    MFGetAttributeRatio(t, MF_MT_FRAME_RATE, &m.fpsNum, &m.fpsDen);
    return m;
}

// Saved device if present, otherwise the most capture-card-looking one.
static int PickDevice(const std::vector<DeviceInfo>& devs, const std::wstring& wanted) {
    if (!wanted.empty())
        for (size_t i = 0; i < devs.size(); ++i)
            if (devs[i].name == wanted) return int(i);
    int best = -1, bestScore = -1;
    for (size_t i = 0; i < devs.size(); ++i) {
        int s = CaptureNameScore(devs[i].name);
        if (s > bestScore) {
            best = int(i);
            bestScore = s;
        }
    }
    return bestScore >= 0 ? best : -1;
}

// Among modes matching the chosen resolution/fps (or all, for automatic):
// smooth (>= 29 fps) first, then up to 1080p, then higher fps, and uncompressed
// over MJPEG on a tie (no decode = less latency) unless it proved too slow.
static int PickMode(const std::vector<VideoMode>& modes, const std::wstring& key,
                    const std::vector<std::wstring>& tooSlow) {
    bool keyExists = std::any_of(modes.begin(), modes.end(), [&](const VideoMode& m) { return m.Key() == key; });
    int best = 0;
    double bestScore = -1e18;
    for (size_t i = 0; i < modes.size(); ++i) {
        const VideoMode& m = modes[i];
        if (keyExists && m.Key() != key) continue;
        const double fps = m.Fps();
        const bool slow = std::find(tooSlow.begin(), tooSlow.end(), m.Id()) != tooSlow.end();
        const double score = (fps >= 29.0 ? 1e9 : 0) + std::min(m.height, 1080u) * 1e5 +
                             std::min(fps, 60.0) * 100 - (m.height > 1080 ? 50 : 0) +
                             (m.Uncompressed() ? (slow ? -10 : 1) : 0);
        if (score > bestScore) {
            bestScore = score;
            best = int(i);
        }
    }
    return best;
}

std::vector<VideoMode> VideoCapture::QualityChoices(const std::vector<VideoMode>& modes) {
    // One entry per resolution/fps, smooth rates only, widescreen when the card offers it.
    auto wide = [](const VideoMode& m) { return std::abs(double(m.width) / m.height - 16.0 / 9.0) < 0.03; };
    const bool anyWide = std::any_of(modes.begin(), modes.end(), wide);
    std::vector<VideoMode> out;
    for (const VideoMode& m : modes) {
        if (m.Fps() < 24.5 || (anyWide && !wide(m))) continue;
        if (std::none_of(out.begin(), out.end(), [&](const VideoMode& o) { return o.Key() == m.Key(); }))
            out.push_back(m);
    }
    std::sort(out.begin(), out.end(), [](const VideoMode& a, const VideoMode& b) {
        return a.height != b.height ? a.height > b.height : a.Fps() > b.Fps();
    });
    return out;
}

static VideoState Classify(HRESULT hr) {
    if (hr == E_ACCESSDENIED) return VideoState::Blocked;
    if (hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) || hr == MF_E_HW_MFT_FAILED_START_STREAMING
#ifdef MF_E_VIDEO_RECORDING_DEVICE_PREEMPTED
        || hr == MF_E_VIDEO_RECORDING_DEVICE_PREEMPTED
#endif
    )
        return VideoState::Busy;
    return VideoState::Error;
}

static void ReadOutputType(IMFSourceReader* reader, Frame& f) {
    ComPtr<IMFMediaType> type;
    if (FAILED(reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &type))) return;
    GUID sub{};
    type->GetGUID(MF_MT_SUBTYPE, &sub);
    MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &f.width, &f.height);
    f.fmt = sub == MFVideoFormat_YUY2    ? PixFmt::YUY2
            : sub == MFVideoFormat_NV12  ? PixFmt::NV12
            : sub == MFVideoFormat_RGB32 ? PixFmt::RGB32
                                         : PixFmt::None;
    f.stride = (LONG)(INT32)MFGetAttributeUINT32(type.Get(), MF_MT_DEFAULT_STRIDE, 0);
    if (f.stride == 0) MFGetStrideForBitmapInfoHeader(sub.Data1, f.width, &f.stride);
    f.nominalRange = MFGetAttributeUINT32(type.Get(), MF_MT_VIDEO_NOMINAL_RANGE, 0);
    f.yuvMatrix = MFGetAttributeUINT32(type.Get(), MF_MT_YUV_MATRIX, 0);
    VideoMode m;
    m.subtype = sub;
    Log(L"video: output %s %ux%u stride %ld, nominal range %u, yuv matrix %u", m.Codec().c_str(), f.width, f.height,
        f.stride, f.nominalRange, f.yuvMatrix);
}

void VideoCapture::Start(FrameFn onFrame, HWND notifyWnd, UINT notifyMsg) {
    onFrame_ = std::move(onFrame);
    notifyWnd_ = notifyWnd;
    notifyMsg_ = notifyMsg;
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread_ = std::thread(&VideoCapture::Run, this);
}

void VideoCapture::Stop() {
    if (!thread_.joinable()) return;
    stop_ = true;
    SetEvent(wake_);
    // Frames arrive every few ms, so the thread normally exits on its own; only
    // yank the source out from under a ReadSample that's genuinely stuck.
    if (WaitForSingleObject(thread_.native_handle(), 500) == WAIT_TIMEOUT) InterruptSource();
    thread_.join();
    CloseHandle(wake_);
    wake_ = nullptr;
}

void VideoCapture::Select(const std::wstring& device, const std::wstring& modeKey) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reqDevice_ = device;
        reqMode_ = modeKey;
    }
    restart_ = true;
    if (wake_) SetEvent(wake_);
}

VideoCapture::Status VideoCapture::GetStatus() {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void VideoCapture::InterruptSource() {
    // Unblocks a pending ReadSample. Must not hold mutex_ while calling
    // Shutdown: the capture thread needs it to unwind.
    ComPtr<IMFMediaSource> source;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        source = source_;
    }
    if (source) source->Shutdown();
}

void VideoCapture::Publish(VideoState s, const DeviceInfo* dev, HRESULT hr) {
    bool changed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::wstring name = dev ? dev->name : L"";
        changed = status_.state != s || status_.device != name;
        status_.state = s;
        status_.device = name;
        status_.symlink = dev ? dev->id : L"";
        status_.lastError = hr;
        if (s != VideoState::Running) {
            status_.modes.clear();
            status_.mode = {};
        }
    }
    if (changed && notifyWnd_) PostMessageW(notifyWnd_, notifyMsg_, 0, 0);
}

void VideoCapture::Run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD task = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Capture", &task);

    while (!stop_) {
        std::wstring wantDev, wantMode;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            wantDev = reqDevice_;
            wantMode = reqMode_;
        }
        restart_ = false;

        std::vector<DeviceInfo> devs = Enumerate();
        int idx = PickDevice(devs, wantDev);
        if (idx < 0) {
            Publish(VideoState::Searching);
            WaitForSingleObject(wake_, 1000);
            continue;
        }
        const DeviceInfo dev = devs[idx];
        Log(L"video: opening \"%s\" (%s)", dev.name.c_str(), dev.id.c_str());
        HRESULT hr = OpenAndStream(dev, wantMode);
        if (stop_ || restart_ || hr == S_FALSE) continue;  // S_FALSE: reopen with a faster mode

        std::vector<DeviceInfo> now = Enumerate();
        bool present = std::any_of(now.begin(), now.end(), [&](const DeviceInfo& d) { return d.id == dev.id; });
        if (!present) {
            Log(L"video: \"%s\" was unplugged", dev.name.c_str());
            continue;
        }
        Log(L"video: stream stopped, hr=0x%08X", hr);
        Publish(Classify(hr), &dev, hr);
        WaitForSingleObject(wake_, 1500);
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    CoUninitialize();
}

HRESULT VideoCapture::OpenAndStream(const DeviceInfo& dev, const std::wstring& modeKey) {
    Publish(VideoState::Opening, &dev);
    ComPtr<IMFAttributes> attr;
    HRESULT hr = MFCreateAttributes(&attr, 2);
    if (FAILED(hr)) return hr;
    attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    attr->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, dev.id.c_str());
    ComPtr<IMFMediaSource> source;
    hr = MFCreateDeviceSource(attr.Get(), &source);
    if (FAILED(hr)) {
        Log(L"video: MFCreateDeviceSource failed, hr=0x%08X", hr);
        return hr;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        source_ = source;
    }
    hr = (restart_ || stop_) ? S_OK : StreamFrom(source.Get(), dev, modeKey);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        source_.Reset();
    }
    source->Shutdown();
    return hr;
}

HRESULT VideoCapture::StreamFrom(IMFMediaSource* source, const DeviceInfo& dev, const std::wstring& modeKey) {
    ComPtr<IMFPresentationDescriptor> pd;
    ComPtr<IMFStreamDescriptor> sd;
    ComPtr<IMFMediaTypeHandler> handler;
    BOOL selected = FALSE;
    HRESULT hr = source->CreatePresentationDescriptor(&pd);
    if (SUCCEEDED(hr)) hr = pd->GetStreamDescriptorByIndex(0, &selected, &sd);
    if (SUCCEEDED(hr)) hr = sd->GetMediaTypeHandler(&handler);
    if (FAILED(hr)) return hr;

    DWORD count = 0;
    handler->GetMediaTypeCount(&count);
    std::vector<VideoMode> modes;
    std::vector<ComPtr<IMFMediaType>> types;
    for (DWORD i = 0; i < count; ++i) {
        ComPtr<IMFMediaType> t;
        if (FAILED(handler->GetMediaTypeByIndex(i, &t))) continue;
        const VideoMode m = ModeFromType(t.Get());
        if (!m.width || !m.height) continue;
        const std::wstring id = m.Id();
        if (std::any_of(modes.begin(), modes.end(), [&](const VideoMode& o) { return o.Id() == id; })) continue;
        modes.push_back(m);
        types.push_back(t);
    }
    if (modes.empty()) return MF_E_INVALIDMEDIATYPE;

    const int idx = PickMode(modes, modeKey, tooSlow_);
    const VideoMode mode = modes[idx];
    hr = handler->SetCurrentMediaType(types[idx].Get());
    if (FAILED(hr)) Log(L"video: SetCurrentMediaType(native) failed, hr=0x%08X", hr);
    Log(L"video: %zu modes, using %s %s", modes.size(), mode.Label().c_str(), mode.Codec().c_str());

    ComPtr<IMFAttributes> readerAttr;
    MFCreateAttributes(&readerAttr, 2);
    readerAttr->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    readerAttr->SetUINT32(MF_LOW_LATENCY, TRUE);
    ComPtr<IMFSourceReader> reader;
    hr = MFCreateSourceReaderFromMediaSource(source, readerAttr.Get(), &reader);
    if (FAILED(hr)) return hr;
    reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

    // Ask for a format the GPU converts itself: the native one when the card
    // sends raw YUY2/NV12 (zero CPU work), otherwise what the MJPEG decoder
    // emits natively. RGB32 is the fallback for anything exotic.
    // The frame rate must be part of the request: without it the reader happily
    // switches the card to a native mode that matches the subtype/size, e.g.
    // YUY2 1080p at 10 fps instead of decoding MJPEG 1080p60.
    std::vector<GUID> outputs;
    if (mode.subtype == MFVideoFormat_YUY2 || mode.subtype == MFVideoFormat_NV12) outputs.push_back(mode.subtype);
    for (const GUID& g : {MFVideoFormat_YUY2, MFVideoFormat_NV12, MFVideoFormat_RGB32})
        if (std::find(outputs.begin(), outputs.end(), g) == outputs.end()) outputs.push_back(g);
    bool haveOutput = false;
    for (const GUID& sub : outputs) {
        ComPtr<IMFMediaType> out;
        MFCreateMediaType(&out);
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out->SetGUID(MF_MT_SUBTYPE, sub);
        MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, mode.width, mode.height);
        MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, mode.fpsNum, mode.fpsDen);
        hr = reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, out.Get());
        if (SUCCEEDED(hr)) {
            haveOutput = true;
            break;
        }
        Log(L"video: output type rejected, hr=0x%08X", hr);
    }
    if (!haveOutput) return hr;
    ComPtr<IMFMediaType> native;
    if (SUCCEEDED(reader->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                             (DWORD)MF_SOURCE_READER_CURRENT_TYPE_INDEX, &native))) {
        const VideoMode n = ModeFromType(native.Get());
        Log(L"video: card is sending %s %s", n.Label().c_str(), n.Codec().c_str());
    }

    Frame proto;
    ReadOutputType(reader.Get(), proto);
    proto.mjpegSource = mode.subtype == MFVideoFormat_MJPG;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.modes = modes;
        status_.mode = mode;
    }
    Publish(VideoState::Running, &dev);
    if (notifyWnd_) PostMessageW(notifyWnd_, notifyMsg_, 0, 0);  // mode list changed even if state didn't

    // Raw modes need a fast USB port. If the first few seconds come in well
    // under the promised rate and a compressed version exists, switch to it.
    const bool canFallBack = mode.Uncompressed() && std::any_of(modes.begin(), modes.end(), [&](const VideoMode& m) {
                                 return m.Key() == mode.Key() && !m.Uncompressed();
                             });
    UINT64 frames = 0;
    ULONGLONG statStart = 0;
    bool checked = !canFallBack;
    while (!stop_ && !restart_) {
        DWORD stream = 0, flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        hr = reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &ts, &sample);
        if (FAILED(hr)) return hr;
        if (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM)) return MF_E_END_OF_STREAM;
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) ReadOutputType(reader.Get(), proto);
        if (!sample) continue;
        Frame f = proto;
        f.sample = std::move(sample);
        onFrame_(std::move(f));

        const ULONGLONG now = GetTickCount64();
        if (!statStart) statStart = now;  // first frame
        ++frames;
        const double elapsed = double(now - statStart), fps = frames * 1000.0 / std::max(elapsed, 1.0);
        if (!checked && elapsed >= 3000) {
            checked = true;
            if (fps < mode.Fps() * 0.75) {
                Log(L"video: only %.1f of %.0f fps in %s, switching to compressed", fps, mode.Fps(),
                    mode.Codec().c_str());
                tooSlow_.push_back(mode.Id());
                return S_FALSE;
            }
        }
        if (elapsed >= 10000) {
            Log(L"video: %.1f fps", fps);
            frames = 0;
            statStart = now;
        }
    }
    return S_OK;
}
