#include "renderer.h"
#include <d3dcompiler.h>
#include <wincodec.h>
#include <mfapi.h>
#include <avrt.h>
#include <algorithm>
#include <cmath>

static const char kShaderSrc[] = R"hlsl(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

Texture2D tex0 : register(t0);
Texture2D tex1 : register(t1);
SamplerState samp0 : register(s0);

cbuffer Params : register(b0) {
    float4 range;  // x: luma offset, y: luma scale, z: chroma offset, w: chroma scale
    float4 coef;   // x: R<-V, y: G<-U, z: G<-V, w: B<-U
    float4 size;   // x: last packed YUY2 texel in a row, zw: video width/height
};

float4 YuvToRgb(float y, float2 uv) {
    y = (y - range.x) * range.y;
    uv = (uv - range.z) * range.w;
    float3 rgb = float3(y + coef.x * uv.y, y - coef.y * uv.x - coef.z * uv.y, y + coef.w * uv.x);
    return float4(saturate(rgb), 1.0);
}

// ---- conversion pass: upload texture(s) -> RGB texture at video size ----

// YUY2 arrives as RGBA8 at half width: one texel = (Y0, U, Y1, V) for two pixels.
float4 PSYuy2(VSOut i) : SV_Target {
    int2 p = int2(i.pos.xy);
    int tx = p.x >> 1;
    float4 t = tex0.Load(int3(tx, p.y, 0));
    bool odd = (p.x & 1) != 0;
    float2 uv = t.ga;
    if (odd) uv = 0.5 * (uv + tex0.Load(int3(min(tx + 1, (int)size.x), p.y, 0)).ga);
    return YuvToRgb(odd ? t.b : t.r, uv);
}

// NV12: full-res R8 luma plane in tex0, half-res R8G8 chroma plane in tex1.
float4 PSNv12(VSOut i) : SV_Target {
    return YuvToRgb(tex0.Load(int3(int2(i.pos.xy), 0)).r, tex1.Sample(samp0, i.uv).rg);
}

float4 PSRgb(VSOut i) : SV_Target {
    return float4(tex0.Load(int3(int2(i.pos.xy), 0)).rgb, 1.0);
}

// ---- scaling pass: RGB texture -> window ----

float4 PSCopy(VSOut i) : SV_Target {
    return float4(tex0.Sample(samp0, i.uv).rgb, 1.0);
}

// Catmull-Rom bicubic: noticeably crisper than bilinear when upscaling.
float4 PSBicubic(VSOut i) : SV_Target {
    float2 p = i.uv * size.zw - 0.5;
    float2 f = frac(p);
    int2 base = int2(floor(p));
    float2 w[4];
    w[0] = f * (-0.5 + f * (1.0 - 0.5 * f));
    w[1] = 1.0 + f * f * (-2.5 + 1.5 * f);
    w[2] = f * (0.5 + f * (2.0 - 1.5 * f));
    w[3] = f * f * (-0.5 + 0.5 * f);
    int2 maxPos = int2(size.zw) - 1;
    float3 c = 0;
    [unroll] for (int y = 0; y < 4; ++y)
        [unroll] for (int x = 0; x < 4; ++x)
            c += tex0.Load(int3(clamp(base + int2(x - 1, y - 1), int2(0, 0), maxPos), 0)).rgb * (w[x].x * w[y].y);
    return float4(saturate(c), 1.0);
}
)hlsl";

struct alignas(16) ShaderParams {
    float range[4];
    float coef[4];
    float size[4];
};

// Segoe Fluent Icons (Windows 11) / Segoe MDL2 Assets (Windows 10) glyphs.
namespace Glyph {
const wchar_t Volume0[] = L"", Volume1[] = L"", Volume3[] = L"", Mute[] = L"";
const wchar_t Camera[] = L"", Info[] = L"", Pin[] = L"", Settings[] = L"";
const wchar_t FullScreen[] = L"", BackToWindow[] = L"", Monitor[] = L"";
}  // namespace Glyph

static const D2D1_COLOR_F kAccent = {1.0f, 0.33f, 0.38f, 1.0f};  // Joy-Con red

static D2D1_COLOR_F White(float a) { return {1, 1, 1, a}; }
static D2D1_RECT_F ToD2D(const RectF& r) { return {r.l, r.t, r.r, r.b}; }

static HRESULT Compile(const char* entry, const char* target, ComPtr<ID3DBlob>& out) {
    ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(kShaderSrc, sizeof(kShaderSrc) - 1, "capview.hlsl", nullptr, nullptr, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &errors);
    if (FAILED(hr)) Log(L"render: shader %S failed: %S", entry, errors ? (const char*)errors->GetBufferPointer() : "");
    return hr;
}

// ---------------------------------------------------------------- public API

bool Renderer::Init(HWND hwnd) {
    hwnd_ = hwnd;
    // Device + swap chain are created on the window's thread; afterwards only
    // the render thread touches the immediate context.
    if (!CreateDevice() || !CreateSizeDependent()) return false;
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread_ = std::thread(&Renderer::Run, this);
    return true;
}

void Renderer::Shutdown() {
    if (!thread_.joinable()) return;
    stop_ = true;
    SetEvent(wake_);
    // Keep pumping messages while waiting: DXGI may need the window thread.
    HANDLE h = thread_.native_handle();
    while (MsgWaitForMultipleObjects(1, &h, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0 + 1) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    }
    thread_.join();
    CloseHandle(wake_);
    wake_ = nullptr;
}

template <class F> void Renderer::Update(F&& change) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        change(shared_);
    }
    if (wake_) SetEvent(wake_);
}

void Renderer::SubmitFrame(Frame&& f) { Update([&](Shared& s) { s.frame = std::move(f); }); }
void Renderer::Resize() { Update([](Shared& s) { s.resize = true; }); }
void Renderer::ClearVideo() { Update([](Shared& s) { s.clear = true; s.frame = Frame{}; }); }
void Renderer::RequestScreenshot() { Update([](Shared& s) { s.screenshot = true; }); }
void Renderer::SetUi(const UiState& ui) { Update([&](Shared& s) { s.ui = ui; }); }

void Renderer::SetStatus(const std::wstring& title, const std::wstring& subtitle) {
    Update([&](Shared& s) {
        s.title = title;
        s.sub = subtitle;
    });
}

void Renderer::ShowOsd(const std::wstring& text) {
    Update([&](Shared& s) {
        s.osd = text;
        s.osdUntil = GetTickCount64() + 1800;
    });
}

void Renderer::SetOptions(bool keepAspect, bool allowTearing, Scaling scaling) {
    Update([&](Shared& s) {
        s.keepAspect = keepAspect;
        s.tearing = allowTearing;
        s.scaling = scaling;
    });
}

// ---------------------------------------------------------------- device setup

bool Renderer::CreateDevice() {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 4, D3D11_SDK_VERSION,
                                   &dev_, nullptr, &ctx_);
    if (hr == E_INVALIDARG)  // pre-11.1 runtimes reject the 11_1 level
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels + 1, 3, D3D11_SDK_VERSION,
                               &dev_, nullptr, &ctx_);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels + 1, 3, D3D11_SDK_VERSION, &dev_,
                               nullptr, &ctx_);
    if (FAILED(hr)) {
        Log(L"render: D3D11CreateDevice failed, hr=0x%08X", hr);
        return false;
    }

    ComPtr<IDXGIDevice1> dxgiDev;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    dev_.As(&dxgiDev);
    dxgiDev->GetAdapter(&adapter);
    adapter->GetParent(IID_PPV_ARGS(&factory));
    dxgiDev->SetMaximumFrameLatency(1);
    DXGI_ADAPTER_DESC ad{};
    adapter->GetDesc(&ad);
    Log(L"render: GPU %s", ad.Description);

    ComPtr<IDXGIFactory5> factory5;
    BOOL tearing = FALSE;
    if (SUCCEEDED(factory.As(&factory5)))
        factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
    tearingSupported_ = tearing != FALSE;
    swapFlags_ = tearingSupported_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = swapFlags_;
    hr = factory->CreateSwapChainForHwnd(dev_.Get(), hwnd_, &sd, nullptr, nullptr, &swap_);
    if (FAILED(hr)) {
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;  // Windows 8.1
        sd.Flags = swapFlags_ = 0;
        tearingSupported_ = false;
        hr = factory->CreateSwapChainForHwnd(dev_.Get(), hwnd_, &sd, nullptr, nullptr, &swap_);
    }
    if (FAILED(hr)) {
        Log(L"render: CreateSwapChainForHwnd failed, hr=0x%08X", hr);
        return false;
    }
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);

    ComPtr<ID3DBlob> blob;
    if (FAILED(Compile("VSMain", "vs_4_0", blob))) return false;
    dev_->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &vs_);
    const struct { const char* entry; ComPtr<ID3D11PixelShader>* out; } pixelShaders[] = {
        {"PSYuy2", &psYuy2_}, {"PSNv12", &psNv12_}, {"PSRgb", &psRgb_}, {"PSCopy", &psCopy_},
        {"PSBicubic", &psBicubic_},
    };
    for (auto& ps : pixelShaders) {
        if (FAILED(Compile(ps.entry, "ps_4_0", blob))) return false;
        dev_->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, ps.out->ReleaseAndGetAddressOf());
    }

    D3D11_SAMPLER_DESC samp{};
    samp.AddressU = samp.AddressV = samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.MaxLOD = D3D11_FLOAT32_MAX;
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    dev_->CreateSamplerState(&samp, &linear_);
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    dev_->CreateSamplerState(&samp, &point_);

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(ShaderParams);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    dev_->CreateBuffer(&cb, nullptr, &params_);

    if (!d2d_) {
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf());
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()));
        ComPtr<IDWriteFontCollection> fonts;
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (dwrite_ && SUCCEEDED(dwrite_->GetSystemFontCollection(&fonts)))
            fonts->FindFamilyName(L"Segoe Fluent Icons", &index, &exists);
        iconFont_ = exists ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    }
    return vs_ && psBicubic_ && linear_ && point_ && params_ && d2d_ && dwrite_;
}

bool Renderer::CreateSizeDependent() {
    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) return false;
    dev_->CreateRenderTargetView(backBuffer.Get(), nullptr, &rtv_);
    D3D11_TEXTURE2D_DESC desc{};
    backBuffer->GetDesc(&desc);
    bbW_ = desc.Width;
    bbH_ = desc.Height;

    ComPtr<IDXGISurface> surface;
    backBuffer.As(&surface);
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);  // draw in physical pixels; we scale by DPI ourselves
    HRESULT hr = d2d_->CreateDxgiSurfaceRenderTarget(surface.Get(), &props, &d2dTarget_);
    if (FAILED(hr)) {
        Log(L"render: CreateDxgiSurfaceRenderTarget failed, hr=0x%08X", hr);
        return false;
    }
    d2dTarget_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    d2dTarget_->CreateSolidColorBrush(White(1), &brush_);

    scale_ = std::max(1.0f, GetDpiForWindow(hwnd_) / 96.0f);
    if (scale_ != fontScale_) {  // fonts only depend on DPI
        fontScale_ = scale_;
        auto make = [&](const wchar_t* family, DWRITE_FONT_WEIGHT weight, float size, bool center,
                        ComPtr<IDWriteTextFormat>& out) {
            out.Reset();
            dwrite_->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                      size * scale_, L"en-us", &out);
            if (out && center) {
                out->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
        };
        make(L"Segoe UI Variable Display", DWRITE_FONT_WEIGHT_SEMI_BOLD, 22, true, titleFmt_);
        make(L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL, 14, true, bodyFmt_);
        make(L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL, 13, false, smallFmt_);
        make(L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 15, false, osdFmt_);
        make(iconFont_.c_str(), DWRITE_FONT_WEIGHT_NORMAL, 17, true, iconFmt_);
        make(iconFont_.c_str(), DWRITE_FONT_WEIGHT_NORMAL, 52, true, bigIconFmt_);
        bodyFmt_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    return true;
}

void Renderer::ReleaseSizeDependent() {
    brush_.Reset();
    d2dTarget_.Reset();
    rtv_.Reset();
    ctx_->ClearState();
    ctx_->Flush();
}

void Renderer::RecoverDevice() {
    Log(L"render: GPU device lost, recreating");
    ReleaseSizeDependent();
    ReleaseTextures();
    hasVideo_ = false;
    swap_.Reset();
    ctx_.Reset();
    dev_.Reset();
    if (!CreateDevice() || !CreateSizeDependent()) Log(L"render: device recovery failed");
}

void Renderer::ReleaseTextures() {
    upload_.Reset();
    uploadUV_.Reset();
    converted_.Reset();
    uploadSrv_.Reset();
    uploadUVSrv_.Reset();
    convertedSrv_.Reset();
    convertedRtv_.Reset();
    texW_ = texH_ = 0;
    texFmt_ = PixFmt::None;
}

bool Renderer::EnsureTextures(UINT w, UINT h, PixFmt fmt) {
    if (w == texW_ && h == texH_ && fmt == texFmt_ && upload_) return true;
    ReleaseTextures();

    D3D11_TEXTURE2D_DESC d{};
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    d.Width = w;
    d.Height = h;
    d.Format = fmt == PixFmt::YUY2 ? DXGI_FORMAT_R8G8B8A8_UNORM
               : fmt == PixFmt::NV12 ? DXGI_FORMAT_R8_UNORM
                                     : DXGI_FORMAT_B8G8R8A8_UNORM;
    if (fmt == PixFmt::YUY2) d.Width = w / 2;
    if (FAILED(dev_->CreateTexture2D(&d, nullptr, &upload_))) return false;
    dev_->CreateShaderResourceView(upload_.Get(), nullptr, &uploadSrv_);

    if (fmt == PixFmt::NV12) {
        D3D11_TEXTURE2D_DESC uv = d;
        uv.Width = w / 2;
        uv.Height = h / 2;
        uv.Format = DXGI_FORMAT_R8G8_UNORM;
        if (FAILED(dev_->CreateTexture2D(&uv, nullptr, &uploadUV_))) return false;
        dev_->CreateShaderResourceView(uploadUV_.Get(), nullptr, &uploadUVSrv_);
    }

    D3D11_TEXTURE2D_DESC c = d;
    c.Width = w;
    c.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    c.Usage = D3D11_USAGE_DEFAULT;
    c.CPUAccessFlags = 0;
    c.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(dev_->CreateTexture2D(&c, nullptr, &converted_))) return false;
    dev_->CreateShaderResourceView(converted_.Get(), nullptr, &convertedSrv_);
    dev_->CreateRenderTargetView(converted_.Get(), nullptr, &convertedRtv_);

    texW_ = w;
    texH_ = h;
    texFmt_ = fmt;
    Log(L"render: textures %ux%u %s", w, h, PixFmtName(fmt));
    return true;
}

// ---------------------------------------------------------------- per frame

void Renderer::DrawPass(ID3D11RenderTargetView* rtv, const D3D11_VIEWPORT& vp, ID3D11PixelShader* ps,
                        ID3D11ShaderResourceView* srv0, ID3D11ShaderResourceView* srv1,
                        ID3D11SamplerState* sampler) {
    ID3D11ShaderResourceView* srvs[2] = {srv0, srv1};
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    ctx_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx_->RSSetState(nullptr);
    ctx_->RSSetViewports(1, &vp);
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->PSSetShader(ps, nullptr, 0);
    ctx_->PSSetConstantBuffers(0, 1, params_.GetAddressOf());
    ctx_->PSSetSamplers(0, 1, &sampler);
    ctx_->PSSetShaderResources(0, 2, srvs);
    ctx_->Draw(3, 0);
    ID3D11ShaderResourceView* none[2] = {};
    ctx_->PSSetShaderResources(0, 2, none);
}

bool Renderer::Upload(const Frame& f) {
    if (f.fmt == PixFmt::None || !f.width || !f.height) return false;
    if (!EnsureTextures(f.width, f.height, f.fmt)) return false;

    ComPtr<IMFMediaBuffer> buffer;
    DWORD count = 0;
    f.sample->GetBufferCount(&count);
    HRESULT hr = count == 1 ? f.sample->GetBufferByIndex(0, &buffer) : f.sample->ConvertToContiguousBuffer(&buffer);
    if (FAILED(hr)) return false;

    const bool nv12 = f.fmt == PixFmt::NV12;
    const UINT rowBytes = f.fmt == PixFmt::YUY2 ? f.width * 2 : nv12 ? f.width : f.width * 4;
    BYTE* scan0 = nullptr;
    LONG pitch = 0;
    DWORD length = 0;
    ComPtr<IMF2DBuffer> buffer2d;
    const bool locked2d = SUCCEEDED(buffer.As(&buffer2d)) && SUCCEEDED(buffer2d->Lock2D(&scan0, &pitch));
    if (locked2d) {
        buffer->GetCurrentLength(&length);
    } else {
        BYTE* raw = nullptr;
        DWORD maxLen = 0;
        if (FAILED(buffer->Lock(&raw, &maxLen, &length))) return false;
        pitch = f.stride ? f.stride : LONG(rowBytes);
        scan0 = pitch < 0 ? raw + size_t(f.height - 1) * size_t(-pitch) : raw;  // bottom-up RGB
    }
    // Decoders may pad the luma plane (e.g. 1080 -> 1088 rows); the chroma
    // plane starts after the padded height, which the buffer length reveals.
    UINT planeRows = f.height;
    if (nv12 && pitch > 0) planeRows = std::max<UINT>(f.height, UINT(length / (UINT(pitch) * 3 / 2)));
    const size_t needed = size_t(std::abs(pitch)) * (nv12 ? planeRows + f.height / 2 : f.height);

    auto copyPlane = [&](ID3D11Texture2D* tex, const BYTE* src, UINT rows, UINT bytes) -> HRESULT {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT h = ctx_->Map(tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(h)) return h;
        BYTE* dst = static_cast<BYTE*>(mapped.pData);
        for (UINT y = 0; y < rows; ++y) memcpy(dst + size_t(y) * mapped.RowPitch, src + ptrdiff_t(y) * pitch, bytes);
        ctx_->Unmap(tex, 0);
        return S_OK;
    };
    hr = (length && needed > length) ? E_UNEXPECTED : copyPlane(upload_.Get(), scan0, f.height, rowBytes);
    if (SUCCEEDED(hr) && nv12)
        hr = copyPlane(uploadUV_.Get(), scan0 + size_t(pitch) * planeRows, f.height / 2, f.width);
    if (locked2d) buffer2d->Unlock2D();
    else buffer->Unlock();
    if (FAILED(hr)) return false;

    // Trust the colorimetry the card/decoder reports; otherwise MJPEG (JFIF) is
    // full-range BT.601 and uncompressed HD is limited-range BT.709.
    const bool full = f.nominalRange == MFNominalRange_0_255     ? true
                      : f.nominalRange == MFNominalRange_16_235 ? false
                                                                : f.mjpegSource;
    const bool bt709 = f.yuvMatrix == MFVideoTransferMatrix_BT709  ? true
                       : f.yuvMatrix == MFVideoTransferMatrix_BT601 ? false
                                                                    : (!f.mjpegSource && f.height >= 720);
    ShaderParams p = {
        {full ? 0.0f : 16.0f / 255, full ? 1.0f : 255.0f / 219, 128.0f / 255, full ? 1.0f : 255.0f / 224},
        {bt709 ? 1.5748f : 1.402f, bt709 ? 0.187324f : 0.344136f, bt709 ? 0.468124f : 0.714136f,
         bt709 ? 1.8556f : 1.772f},
        {float(f.width / 2 - 1), 0, float(f.width), float(f.height)},
    };
    D3D11_MAPPED_SUBRESOURCE cb{};
    if (SUCCEEDED(ctx_->Map(params_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &cb))) {
        memcpy(cb.pData, &p, sizeof(p));
        ctx_->Unmap(params_.Get(), 0);
    }
    ID3D11PixelShader* ps = f.fmt == PixFmt::YUY2 ? psYuy2_.Get() : nv12 ? psNv12_.Get() : psRgb_.Get();
    DrawPass(convertedRtv_.Get(), {0, 0, float(f.width), float(f.height), 0, 1}, ps, uploadSrv_.Get(),
             uploadUVSrv_.Get(), linear_.Get());
    return true;
}

void Renderer::Draw(float osdAlpha) {
    if (!rtv_) return;
    const float black[4] = {0, 0, 0, 1};
    ctx_->ClearRenderTargetView(rtv_.Get(), black);

    if (hasVideo_) {
        float x = 0, y = 0, w = float(bbW_), h = float(bbH_);
        if (cur_.keepAspect) {
            float s = std::min(w / texW_, h / texH_);
            float vw = std::round(texW_ * s), vh = std::round(texH_ * s);
            x = std::floor((w - vw) / 2);
            y = std::floor((h - vh) / 2);
            w = vw;
            h = vh;
        }
        const bool upscaling = w > texW_ || h > texH_;
        ID3D11PixelShader* ps = psCopy_.Get();
        ID3D11SamplerState* sampler = linear_.Get();
        if (cur_.scaling == Scaling::Pixel) sampler = point_.Get();
        else if (cur_.scaling == Scaling::Sharp && upscaling) ps = psBicubic_.Get();
        DrawPass(rtv_.Get(), {x, y, w, h, 0, 1}, ps, convertedSrv_.Get(), nullptr, sampler);
    }

    DrawOverlay(osdAlpha);

    const UINT flags = (cur_.tearing && tearingSupported_) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    HRESULT hr = swap_->Present(0, flags);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) RecoverDevice();
}

// ---------------------------------------------------------------- overlay (Direct2D)

void Renderer::DrawText(const std::wstring& text, IDWriteTextFormat* fmt, float l, float t, float r, float b,
                        D2D1_COLOR_F color) {
    brush_->SetColor(color);
    d2dTarget_->DrawText(text.c_str(), UINT32(text.size()), fmt, D2D1::RectF(l, t, r, b), brush_.Get());
}

// Text on a dark rounded background, anchored at its top-left (or top-right) corner.
void Renderer::DrawPill(const std::wstring& text, IDWriteTextFormat* fmt, float x, float y, float alpha,
                        bool alignRight) {
    ComPtr<IDWriteTextLayout> layout;
    if (text.empty() || FAILED(dwrite_->CreateTextLayout(text.c_str(), UINT32(text.size()), fmt, float(bbW_),
                                                         float(bbH_), &layout)))
        return;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    const float s = scale_, padX = 12 * s, padY = 7 * s, w = m.width + padX * 2, h = m.height + padY * 2;
    if (alignRight) x -= w;
    x = std::clamp(x, 8 * s, std::max(8 * s, bbW_ - w - 8 * s));
    brush_->SetColor({0.06f, 0.06f, 0.08f, 0.78f * alpha});
    d2dTarget_->FillRoundedRectangle({D2D1::RectF(x, y, x + w, y + h), 8 * s, 8 * s}, brush_.Get());
    brush_->SetColor(White(0.95f * alpha));
    d2dTarget_->DrawTextLayout(D2D1::Point2F(x + padX, y + padY), layout.Get(), brush_.Get());
}

void Renderer::DrawBar() {
    const float s = scale_, a = barAlpha_;
    const UiState& ui = cur_.ui;
    const UiLayout L = LayoutBar(float(bbW_), float(bbH_), s);
    const float radius = L.bar.H() / 2;

    brush_->SetColor({0.06f, 0.06f, 0.08f, 0.72f * a});
    d2dTarget_->FillRoundedRectangle({ToD2D(L.bar), radius, radius}, brush_.Get());
    brush_->SetColor(White(0.08f * a));
    d2dTarget_->DrawRoundedRectangle({ToD2D(L.bar), radius, radius}, brush_.Get(), 1.0f);

    for (const UiItem& item : L.items) {
        const RectF& r = item.rect;
        const float cx = (r.l + r.r) / 2, cy = (r.t + r.b) / 2;
        if (item.id == UiId::Volume) {
            const RectF& t = L.track;
            const float knobX = t.l + t.W() * ui.volume, h = t.H() / 2;
            brush_->SetColor(White(0.22f * a));
            d2dTarget_->FillRoundedRectangle({ToD2D(t), h, h}, brush_.Get());
            brush_->SetColor(ui.muted ? White(0.45f * a) : White(0.95f * a));
            d2dTarget_->FillRoundedRectangle({D2D1::RectF(t.l, t.t, knobX, t.b), h, h}, brush_.Get());
            const float knob = (ui.hover == UiId::Volume ? 7.0f : 6.0f) * s;
            d2dTarget_->FillEllipse({{knobX, cy}, knob, knob}, brush_.Get());
            continue;
        }
        if (ui.hover == item.id) {
            brush_->SetColor(White((ui.pressed == item.id ? 0.24f : 0.13f) * a));
            const float rr = r.H() / 2 - 2 * s;
            d2dTarget_->FillEllipse({{cx, cy}, rr, rr}, brush_.Get());
        }
        const wchar_t* glyph = L"";
        bool on = false;
        switch (item.id) {
            case UiId::Mute:
                glyph = ui.muted ? Glyph::Mute : ui.volume < 0.01f ? Glyph::Volume0
                                             : ui.volume < 0.5f    ? Glyph::Volume1
                                                                   : Glyph::Volume3;
                on = ui.muted;
                break;
            case UiId::Screenshot: glyph = Glyph::Camera; break;
            case UiId::Stats: glyph = Glyph::Info; on = ui.stats; break;
            case UiId::Pin: glyph = Glyph::Pin; on = ui.onTop; break;
            case UiId::Settings: glyph = Glyph::Settings; break;
            case UiId::Fullscreen: glyph = ui.fullscreen ? Glyph::BackToWindow : Glyph::FullScreen; break;
            default: break;
        }
        D2D1_COLOR_F color = on ? kAccent : White(0.92f);
        color.a *= a;
        DrawText(glyph, iconFmt_.Get(), r.l, r.t, r.r, r.b, color);
    }

    // Tooltip above the hovered control, so every button explains itself.
    if (ui.hover != UiId::None) {
        for (const UiItem& item : L.items) {
            if (item.id != ui.hover) continue;
            const std::wstring tip = Tooltip(item.id, ui);
            ComPtr<IDWriteTextLayout> layout;
            if (FAILED(dwrite_->CreateTextLayout(tip.c_str(), UINT32(tip.size()), smallFmt_.Get(), float(bbW_),
                                                 float(bbH_), &layout)))
                break;
            DWRITE_TEXT_METRICS m{};
            layout->GetMetrics(&m);
            const float w = m.width + 24 * s, h = m.height + 14 * s;
            DrawPill(tip, smallFmt_.Get(), (item.rect.l + item.rect.r) / 2 - w / 2, L.bar.t - 8 * s - h, a, false);
        }
    }
}

void Renderer::DrawStats() {
    std::wstring text = cur_.ui.statsText;
    if (hasVideo_) {
        wchar_t line[128];
        swprintf_s(line, L"\nShowing %.1f fps  ·  %s scaling%s", fps_,
                   cur_.scaling == Scaling::Sharp ? L"sharp" : cur_.scaling == Scaling::Smooth ? L"smooth" : L"pixel",
                   cur_.tearing && tearingSupported_ ? L"  ·  tearing on" : L"");
        text += line;
    }
    DrawPill(text, smallFmt_.Get(), float(bbW_) - 16 * scale_, 16 * scale_, 1.0f, true);
}

void Renderer::DrawOverlay(float osdAlpha) {
    const bool showBar = barAlpha_ > 0.01f, showStats = cur_.ui.stats;
    if (!d2dTarget_ || (hasVideo_ && !showBar && !showStats && osdAlpha <= 0)) return;
    const float s = scale_, W = float(bbW_), H = float(bbH_);
    d2dTarget_->BeginDraw();

    if (!hasVideo_) {
        const float mid = H * 0.45f;
        DrawText(Glyph::Monitor, bigIconFmt_.Get(), 0, mid - 110 * s, W, mid - 30 * s, White(0.35f));
        DrawText(cur_.title, titleFmt_.Get(), 24 * s, mid - 24 * s, W - 24 * s, mid + 12 * s, White(0.92f));
        DrawText(cur_.sub, bodyFmt_.Get(), 48 * s, mid + 20 * s, W - 48 * s, mid + 100 * s, White(0.55f));
    }
    if (showStats) DrawStats();
    if (osdAlpha > 0) DrawPill(cur_.osd, osdFmt_.Get(), 16 * s, 16 * s, osdAlpha, false);
    if (showBar) DrawBar();

    if (d2dTarget_->EndDraw() == D2DERR_RECREATE_TARGET) {
        ReleaseSizeDependent();
        CreateSizeDependent();
    }
}

// ---------------------------------------------------------------- screenshots

void Renderer::SaveScreenshot() {
    std::wstring message = L"Nothing to screenshot yet";
    if (hasVideo_) {
        message = L"Screenshot failed (see log)";
        D3D11_TEXTURE2D_DESC d{};
        converted_->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(dev_->CreateTexture2D(&d, nullptr, &staging))) {
            ctx_->CopyResource(staging.Get(), converted_.Get());
            if (SUCCEEDED(ctx_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                SYSTEMTIME t;
                GetLocalTime(&t);
                wchar_t name[64];
                swprintf_s(name, L"\\CapView %04u-%02u-%02u %02u.%02u.%02u.png", t.wYear, t.wMonth, t.wDay, t.wHour,
                           t.wMinute, t.wSecond);
                const std::wstring path = ScreenshotDir() + name;

                ComPtr<IWICImagingFactory> wic;
                ComPtr<IWICStream> stream;
                ComPtr<IWICBitmapEncoder> encoder;
                ComPtr<IWICBitmapFrameEncode> frame;
                WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
                HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                              IID_PPV_ARGS(&wic));
                if (SUCCEEDED(hr)) hr = wic->CreateStream(&stream);
                if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
                if (SUCCEEDED(hr)) hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
                if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
                if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
                if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
                if (SUCCEEDED(hr)) hr = frame->SetSize(d.Width, d.Height);
                if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
                if (SUCCEEDED(hr))
                    hr = frame->WritePixels(d.Height, mapped.RowPitch, mapped.RowPitch * d.Height,
                                            static_cast<BYTE*>(mapped.pData));
                if (SUCCEEDED(hr)) hr = frame->Commit();
                if (SUCCEEDED(hr)) hr = encoder->Commit();
                ctx_->Unmap(staging.Get(), 0);
                if (SUCCEEDED(hr)) message = L"Screenshot saved to Pictures › CapView";
                Log(L"screenshot: %s hr=0x%08X", path.c_str(), hr);
            }
        }
    }
    ShowOsd(message);
}

// ---------------------------------------------------------------- render loop

void Renderer::Run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD task = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Playback", &task);
    lastTick_ = fpsStart_ = GetTickCount64();

    while (!stop_) {
        const float barTarget = cur_.ui.barVisible ? 1.0f : 0.0f;
        const bool animating = cur_.osdUntil || barAlpha_ != barTarget;
        WaitForSingleObject(wake_, animating ? 15 : INFINITE);
        if (stop_) break;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            cur_ = shared_;  // copies strings/ui; takes a reference to the frame
            shared_.frame = Frame{};
            shared_.resize = shared_.clear = shared_.screenshot = false;
        }
        const ULONGLONG now = GetTickCount64();
        const float dt = float(now - lastTick_) / 1000.0f;
        lastTick_ = now;
        const float target = cur_.ui.barVisible ? 1.0f : 0.0f;
        barAlpha_ = target > barAlpha_ ? std::min(target, barAlpha_ + dt / 0.12f)
                                       : std::max(target, barAlpha_ - dt / 0.25f);
        float osdAlpha = 0;
        if (cur_.osdUntil > now) osdAlpha = std::min(1.0f, float(cur_.osdUntil - now) / 400.0f);
        else cur_.osdUntil = 0;

        if (cur_.resize) {
            ReleaseSizeDependent();
            HRESULT hr = swap_->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, swapFlags_);
            if (FAILED(hr)) Log(L"render: ResizeBuffers failed, hr=0x%08X", hr);
            CreateSizeDependent();
        }
        if (cur_.clear) hasVideo_ = false;
        if (cur_.frame.sample) {
            if (Upload(cur_.frame)) {
                hasVideo_ = true;
                ++frameCount_;
            }
            cur_.frame = Frame{};  // hand the buffer back to the capture pipeline asap
        }
        if (now - fpsStart_ >= 1000) {
            fps_ = frameCount_ * 1000.0f / float(now - fpsStart_);
            frameCount_ = 0;
            fpsStart_ = now;
        }
        Draw(osdAlpha);
        if (cur_.screenshot) SaveScreenshot();
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    CoUninitialize();
}
