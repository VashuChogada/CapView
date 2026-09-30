#include "audio.h"
#include <mmreg.h>
#include <ksmedia.h>
#include <avrt.h>
#include <algorithm>

static const PROPERTYKEY kFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
static const PROPERTYKEY kContainerId = {
    {0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};

// Buffering policy (in ms of audio queued on the output device):
// after an underrun we pad with this much silence to absorb scheduling jitter...
static const UINT32 kCushionMs = 15;
// ...and if clock drift piles up more than cushion + this, we skip ahead.
static const UINT32 kDriftMs = 30;

namespace {

struct Endpoint {
    std::wstring id, name;
    GUID container{};
    bool hasContainer = false;
};

std::vector<Endpoint> ListEndpoints(IMMDeviceEnumerator* en, EDataFlow flow) {
    std::vector<Endpoint> out;
    ComPtr<IMMDeviceCollection> col;
    if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col))) return out;
    UINT n = 0;
    col->GetCount(&n);
    for (UINT i = 0; i < n; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(col->Item(i, &dev))) continue;
        Endpoint e;
        LPWSTR id = nullptr;
        if (SUCCEEDED(dev->GetId(&id))) e.id = id;
        CoTaskMemFree(id);
        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(props->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR) e.name = v.pwszVal;
            PropVariantClear(&v);
            if (SUCCEEDED(props->GetValue(kContainerId, &v)) && v.vt == VT_CLSID && v.puuid) {
                e.container = *v.puuid;
                e.hasContainer = true;
            }
            PropVariantClear(&v);
        }
        out.push_back(std::move(e));
    }
    return out;
}

std::wstring EndpointName(IMMDevice* dev) {
    std::wstring name;
    ComPtr<IPropertyStore> props;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR) name = v.pwszVal;
        PropVariantClear(&v);
    }
    return name;
}

// Explicit choice by name, else the endpoint on the same physical device as the
// video input, else the most capture-card-looking name.
ComPtr<IMMDevice> ResolveInput(IMMDeviceEnumerator* en, const std::wstring& wanted, const std::wstring& hint) {
    std::vector<Endpoint> eps = ListEndpoints(en, eCapture);
    const Endpoint* pick = nullptr;
    if (!wanted.empty())
        for (auto& e : eps)
            if (e.name == wanted) pick = &e;
    GUID container{};
    if (!pick && !hint.empty() && GetContainerId(hint, container))
        for (auto& e : eps)
            if (e.hasContainer && IsEqualGUID(e.container, container)) pick = &e;
    if (!pick) {
        int best = 0;
        for (auto& e : eps) {
            int s = CaptureNameScore(e.name);
            if (s > best) {
                best = s;
                pick = &e;
            }
        }
    }
    ComPtr<IMMDevice> dev;
    if (pick) en->GetDevice(pick->id.c_str(), &dev);
    return dev;
}

std::wstring Describe(const WAVEFORMATEX* f) {
    wchar_t buf[128];
    bool isFloat = f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                   (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                    IsEqualGUID(reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f)->SubFormat,
                                KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
    swprintf_s(buf, L"%lu Hz, %u ch, %u-bit%s", f->nSamplesPerSec, f->nChannels, f->wBitsPerSample,
               isFloat ? L" float" : L"");
    return buf;
}

enum class SampleType { Unknown, Float32, Int16, Int24, Int32 };

SampleType SampleTypeOf(const WAVEFORMATEX* f) {
    bool isFloat = f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    bool isPcm = f->wFormatTag == WAVE_FORMAT_PCM;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22) {
        const GUID& sub = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f)->SubFormat;
        isFloat = IsEqualGUID(sub, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        isPcm = IsEqualGUID(sub, KSDATAFORMAT_SUBTYPE_PCM);
    }
    const UINT32 bytes = f->nChannels ? f->nBlockAlign / f->nChannels : 0;
    if (isFloat && bytes == 4) return SampleType::Float32;
    if (isPcm && bytes == 2) return SampleType::Int16;
    if (isPcm && bytes == 3) return SampleType::Int24;
    if (isPcm && bytes == 4) return SampleType::Int32;
    return SampleType::Unknown;
}

// Multiplies samples in place by gain (> 1), hard-clipping at full scale.
void ApplyGain(BYTE* p, size_t bytes, SampleType type, float gain) {
    switch (type) {
        case SampleType::Float32: {
            float* s = reinterpret_cast<float*>(p);
            for (size_t i = 0, n = bytes / 4; i < n; ++i) s[i] = std::clamp(s[i] * gain, -1.0f, 1.0f);
            break;
        }
        case SampleType::Int16: {
            INT16* s = reinterpret_cast<INT16*>(p);
            for (size_t i = 0, n = bytes / 2; i < n; ++i)
                s[i] = INT16(std::clamp(s[i] * gain, -32768.0f, 32767.0f));
            break;
        }
        case SampleType::Int24:
            for (size_t i = 0; i + 3 <= bytes; i += 3) {
                int v = int(UINT32(p[i]) << 8 | UINT32(p[i + 1]) << 16 | UINT32(p[i + 2]) << 24) >> 8;
                v = int(std::clamp(v * gain, -8388608.0f, 8388607.0f));
                p[i] = BYTE(v), p[i + 1] = BYTE(v >> 8), p[i + 2] = BYTE(v >> 16);
            }
            break;
        case SampleType::Int32: {
            INT32* s = reinterpret_cast<INT32*>(p);
            for (size_t i = 0, n = bytes / 4; i < n; ++i)
                s[i] = INT32(std::clamp(double(s[i]) * gain, -2147483648.0, 2147483647.0));
            break;
        }
        default: break;
    }
}

class DeviceNotifier: public IMMNotificationClient {
public:
    explicit DeviceNotifier(AudioBridge* owner) : owner_(owner) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
            *out = static_cast<IMMNotificationClient*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) owner_->Wake(true);  // follow headphones <-> speakers
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { owner_->Wake(false); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { owner_->Wake(false); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { owner_->Wake(false); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    AudioBridge* owner_;
};

}  // namespace

std::vector<std::wstring> AudioBridge::EnumInputs() {
    std::vector<std::wstring> names;
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return names;
    for (auto& e : ListEndpoints(en.Get(), eCapture)) names.push_back(e.name);
    return names;
}

void AudioBridge::Start(HWND notifyWnd, UINT notifyMsg) {
    notifyWnd_ = notifyWnd;
    notifyMsg_ = notifyMsg;
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread_ = std::thread(&AudioBridge::Run, this);
}

void AudioBridge::Stop() {
    if (!thread_.joinable()) return;
    stop_ = true;
    SetEvent(wake_);
    thread_.join();
    CloseHandle(wake_);
    wake_ = nullptr;
}

void AudioBridge::Wake(bool restart) {
    if (restart) restart_ = true;
    if (wake_) SetEvent(wake_);
}

void AudioBridge::SetInput(const std::wstring& name) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        input_ = name;
    }
    Wake(true);
}

void AudioBridge::SetVideoHint(const std::wstring& symlink) {
    bool restart;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (hint_ == symlink) return;
        hint_ = symlink;
        // Only re-pick if automatic mode hasn't already found an input.
        restart = input_.empty() && status_.state != AudioState::Running;
    }
    Wake(restart);
}

void AudioBridge::SetVolume(float v) {
    volume_ = v;
    volumeDirty_ = true;
    Wake(false);
}

void AudioBridge::SetMute(bool m) {
    muted_ = m;
    volumeDirty_ = true;
    Wake(false);
}

AudioBridge::Status AudioBridge::GetStatus() {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void AudioBridge::Publish(AudioState s, const std::wstring& in, const std::wstring& out) {
    bool changed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        changed = status_.state != s || status_.input != in || status_.output != out;
        status_ = {s, in, out};
    }
    if (changed && notifyWnd_) PostMessageW(notifyWnd_, notifyMsg_, 0, 0);
}

void AudioBridge::Run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD task = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    ComPtr<IMMDeviceEnumerator> en;
    DeviceNotifier notifier(this);
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))))
        en->RegisterEndpointNotificationCallback(&notifier);

    while (en && !stop_) {
        restart_ = false;
        std::wstring wanted, hint;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            wanted = input_;
            hint = hint_;
        }
        if (wanted == L"off") {
            Publish(AudioState::Off);
            WaitForSingleObject(wake_, INFINITE);
            continue;
        }
        ComPtr<IMMDevice> in = ResolveInput(en.Get(), wanted, hint);
        ComPtr<IMMDevice> out;
        en->GetDefaultAudioEndpoint(eRender, eConsole, &out);
        if (!in || !out) {
            Publish(AudioState::NoInput);
            WaitForSingleObject(wake_, 1000);
            continue;
        }
        HRESULT hr = Stream(in.Get(), out.Get());
        if (stop_ || restart_) continue;
        Log(L"audio: stream stopped, hr=0x%08X", hr);
        Publish(AudioState::Error, EndpointName(in.Get()), EndpointName(out.Get()));
        WaitForSingleObject(wake_, 1000);
    }

    if (en) en->UnregisterEndpointNotificationCallback(&notifier);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    CoUninitialize();
}

HRESULT AudioBridge::Stream(IMMDevice* in, IMMDevice* out) {
    const std::wstring inName = EndpointName(in), outName = EndpointName(out);

    ComPtr<IAudioClient> capClient;
    HRESULT hr = in->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &capClient);
    if (FAILED(hr)) return hr;
    WAVEFORMATEX* capFmt = nullptr;
    hr = capClient->GetMixFormat(&capFmt);
    if (FAILED(hr)) return hr;
    std::vector<BYTE> fmtBytes((BYTE*)capFmt, (BYTE*)capFmt + sizeof(WAVEFORMATEX) + capFmt->cbSize);
    CoTaskMemFree(capFmt);
    WAVEFORMATEX* fmt = reinterpret_cast<WAVEFORMATEX*>(fmtBytes.data());
    const UINT32 capBlock = fmt->nBlockAlign;

    hr = capClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, fmt, nullptr);
    if (FAILED(hr)) return hr;
    HANDLE capEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    capClient->SetEventHandle(capEvent);
    ComPtr<IAudioCaptureClient> capture;
    hr = capClient->GetService(IID_PPV_ARGS(&capture));

    // MacroSilicon (MS2109) cards describe their stereo 48 kHz stream as mono
    // 96 kHz. The bytes are identical, so just tell the output the truth.
    std::vector<BYTE> renderFmtBytes = fmtBytes;
    WAVEFORMATEX* rfmt = reinterpret_cast<WAVEFORMATEX*>(renderFmtBytes.data());
    if (rfmt->nChannels == 1 && rfmt->nSamplesPerSec == 96000) {
        Log(L"audio: mono 96 kHz input, treating as stereo 48 kHz (MacroSilicon quirk)");
        rfmt->nChannels = 2;
        rfmt->nSamplesPerSec = 48000;
        rfmt->nBlockAlign = WORD(rfmt->nBlockAlign * 2);
        if (rfmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && rfmt->cbSize >= 22)
            reinterpret_cast<WAVEFORMATEXTENSIBLE*>(rfmt)->dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    }
    const UINT32 block = rfmt->nBlockAlign, rate = rfmt->nSamplesPerSec;

    ComPtr<IAudioClient> renClient;
    ComPtr<IAudioRenderClient> render;
    ComPtr<ISimpleAudioVolume> volume;
    UINT32 bufferFrames = 0;
    if (SUCCEEDED(hr)) hr = out->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &renClient);
    if (SUCCEEDED(hr))
        hr = renClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                   AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                   100 * 10000 /* 100 ms capacity; we keep it mostly empty */, 0, rfmt, nullptr);
    if (SUCCEEDED(hr)) hr = renClient->GetBufferSize(&bufferFrames);
    if (SUCCEEDED(hr)) hr = renClient->GetService(IID_PPV_ARGS(&render));
    if (SUCCEEDED(hr)) renClient->GetService(IID_PPV_ARGS(&volume));
    if (FAILED(hr)) {
        CloseHandle(capEvent);
        return hr;
    }

    Log(L"audio: \"%s\" (%s) -> \"%s\"", inName.c_str(), Describe(fmt).c_str(), outName.c_str());
    const UINT32 cushion = rate * kCushionMs / 1000;
    const UINT32 maxFill = cushion + rate * kDriftMs / 1000;
    const SampleType sampleType = SampleTypeOf(rfmt);
    if (sampleType == SampleType::Unknown) Log(L"audio: unknown sample format, volume boost above 100%% disabled");
    volumeDirty_ = true;
    bool renderStarted = false;
    UINT64 underruns = 0, skips = 0;
    float gain = 1.0f;  // software boost on top of the session volume, which tops out at 1.0

    auto writeFrames = [&](const BYTE* src, UINT32 frames) -> HRESULT {
        BYTE* dst = nullptr;
        HRESULT h = render->GetBuffer(frames, &dst);
        if (FAILED(h)) return h;
        if (src) {
            memcpy(dst, src, size_t(frames) * block);
            if (gain > 1.0f) ApplyGain(dst, size_t(frames) * block, sampleType, gain);
        }
        return render->ReleaseBuffer(frames, src ? 0 : AUDCLNT_BUFFERFLAGS_SILENT);
    };

    auto push = [&](const BYTE* data, UINT32 frames) -> HRESULT {
        UINT32 pad = 0;
        HRESULT h = renClient->GetCurrentPadding(&pad);
        if (FAILED(h)) return h;
        if (pad == 0) {  // start, or the output ran dry
            if (renderStarted) ++underruns;
            if (FAILED(h = writeFrames(nullptr, cushion))) return h;
            pad = cushion;
        } else if (pad + frames > maxFill) {  // input clock running fast: drop to cushion
            UINT32 drop = std::min(frames, pad - std::min(pad, cushion));
            if (data) data += size_t(drop) * block;
            frames -= drop;
            ++skips;
        }
        frames = std::min(frames, bufferFrames - pad);
        return frames ? writeFrames(data, frames) : S_OK;
    };

    hr = capClient->Start();
    Publish(AudioState::Running, inName, outName);
    ULONGLONG statStart = GetTickCount64();

    while (SUCCEEDED(hr) && !stop_ && !restart_) {
        if (volumeDirty_.exchange(false)) {
            const float v = volume_;
            gain = std::max(v, 1.0f);
            if (volume) {
                volume->SetMasterVolume(std::min(v, 1.0f), nullptr);
                volume->SetMute(muted_, nullptr);
            }
        }
        HANDLE waits[2] = {capEvent, wake_};
        DWORD w = WaitForMultipleObjects(2, waits, FALSE, 500);
        if (w != WAIT_OBJECT_0) continue;

        UINT32 packet = 0;
        while (SUCCEEDED(hr = capture->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
            const UINT32 outFrames = UINT32(UINT64(frames) * capBlock / block);
            hr = push((flags & AUDCLNT_BUFFERFLAGS_SILENT) ? nullptr : data, outFrames);
            capture->ReleaseBuffer(frames);
            if (FAILED(hr)) break;
            if (!renderStarted) {
                hr = renClient->Start();
                renderStarted = true;
            }
        }

        ULONGLONG now = GetTickCount64();
        if (now - statStart >= 30000) {
            if (underruns || skips) Log(L"audio: %llu underruns, %llu drift skips in last 30 s", underruns, skips);
            underruns = skips = 0;
            statStart = now;
        }
    }

    capClient->Stop();
    if (renderStarted) renClient->Stop();
    CloseHandle(capEvent);
    return hr;
}
