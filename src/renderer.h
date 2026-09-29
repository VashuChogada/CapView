#pragma once
#include "common.h"
#include "video.h"
#include "ui.h"
#include <d3d11.h>
#include <dxgi1_5.h>
#include <d2d1.h>
#include <dwrite.h>
#include <thread>
#include <mutex>
#include <atomic>

enum class Scaling { Sharp = 0, Smooth = 1, Pixel = 2 };

// Direct3D 11 presenter running on its own thread. Every new frame is uploaded,
// converted to RGB on the GPU and presented immediately (no frame queue).
class Renderer {
public:
    bool Init(HWND hwnd);
    void Shutdown();

    // All of these are safe to call from any thread.
    void SubmitFrame(Frame&& f);  // newest frame wins
    void Resize();
    void SetStatus(const std::wstring& title, const std::wstring& subtitle);  // shown when there's no video
    void ShowOsd(const std::wstring& text);
    void SetOptions(bool keepAspect, bool allowTearing, Scaling scaling);
    void SetUi(const UiState& ui);
    void ClearVideo();
    void RequestScreenshot();

private:
    // Everything other threads can change; the render thread copies it each pass.
    struct Shared {
        Frame frame;
        bool resize = false, clear = false, screenshot = false;
        std::wstring title, sub, osd;
        ULONGLONG osdUntil = 0;
        bool keepAspect = true, tearing = false;
        Scaling scaling = Scaling::Sharp;
        UiState ui;
    };
    template <class F> void Update(F&& change);

    void Run();
    bool CreateDevice();
    bool CreateSizeDependent();
    void ReleaseSizeDependent();
    void ReleaseTextures();
    bool EnsureTextures(UINT w, UINT h, PixFmt fmt);
    bool Upload(const Frame& f);
    void DrawPass(ID3D11RenderTargetView* rtv, const D3D11_VIEWPORT& vp, ID3D11PixelShader* ps,
                  ID3D11ShaderResourceView* srv0, ID3D11ShaderResourceView* srv1, ID3D11SamplerState* sampler);
    void Draw(float osdAlpha);
    void DrawOverlay(float osdAlpha);
    void DrawBar();
    void DrawStats();
    void DrawText(const std::wstring& text, IDWriteTextFormat* fmt, float l, float t, float r, float b,
                  D2D1_COLOR_F color);
    void DrawPill(const std::wstring& text, IDWriteTextFormat* fmt, float x, float y, float alpha, bool alignRight);
    void SaveScreenshot();
    void RecoverDevice();

    HWND hwnd_ = nullptr;
    std::thread thread_;
    HANDLE wake_ = nullptr;
    std::atomic<bool> stop_{false};
    std::mutex mutex_;
    Shared shared_;

    // Render-thread state.
    Shared cur_;
    bool hasVideo_ = false;
    float barAlpha_ = 1.0f;
    ULONGLONG lastTick_ = 0;
    float scale_ = 1.0f, fontScale_ = 0;
    UINT bbW_ = 0, bbH_ = 0;
    UINT texW_ = 0, texH_ = 0;
    PixFmt texFmt_ = PixFmt::None;
    bool tearingSupported_ = false;
    UINT swapFlags_ = 0;
    UINT frameCount_ = 0;
    ULONGLONG fpsStart_ = 0;
    float fps_ = 0;

    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGISwapChain1> swap_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> psYuy2_, psNv12_, psRgb_, psCopy_, psBicubic_;
    ComPtr<ID3D11SamplerState> linear_, point_;
    ComPtr<ID3D11Buffer> params_;
    // upload_ holds packed pixels (YUY2/RGB32) or the NV12 luma plane; uploadUV_ the
    // NV12 chroma plane. Every format is converted into converted_ (RGB, video size).
    ComPtr<ID3D11Texture2D> upload_, uploadUV_, converted_;
    ComPtr<ID3D11ShaderResourceView> uploadSrv_, uploadUVSrv_, convertedSrv_;
    ComPtr<ID3D11RenderTargetView> convertedRtv_;

    ComPtr<ID2D1Factory> d2d_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<ID2D1RenderTarget> d2dTarget_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<IDWriteTextFormat> titleFmt_, bodyFmt_, smallFmt_, osdFmt_, iconFmt_, bigIconFmt_;
    std::wstring iconFont_;
};
