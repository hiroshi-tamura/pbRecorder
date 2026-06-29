#pragma once

#include "capture/ICaptureSource.h"

#include <d3d11.h>
#include <wrl/client.h>
#include <windows.h>
#include <dwmapi.h>

#include <thread>
#include <atomic>
#include <mutex>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace pb {

class WindowCapture : public ICaptureSource {
public:
    WindowCapture();
    ~WindowCapture() override;

    bool initialize(const CaptureConfig& config, ID3D11Device* device) override;
    bool start() override;
    bool stop() override;
    void setFrameCallback(FrameCallback callback) override;
    void setErrorCallback(ErrorCallback callback) override;
    uint32_t getWidth() const override;
    uint32_t getHeight() const override;
    bool isCapturing() const override;

private:
    void captureLoop();
    bool captureFrame();
    bool createGdiResources(int width, int height);
    void releaseGdiResources();
    ComPtr<ID3D11Texture2D> bitmapToTexture(int offsetX, int offsetY, int cropW, int cropH);
    void compositeCursor(HDC hdc, const RECT& windowRect);
    bool isWindowValid() const;
    bool getWindowRect(RECT& rect) const;
    // Computes the full window rect (PrintWindow's coordinate space) plus the
    // visible content sub-rectangle (DWM extended frame bounds). The invisible
    // resize-border offset between the two is returned via offsetX/offsetY so the
    // captured content can be cropped to the visible window without shifting.
    bool getCaptureGeometry(RECT& fullRect, int& cropW, int& cropH,
                            int& offsetX, int& offsetY) const;
    int64_t queryTimestamp() const;
    void reportError(const std::string& msg);

    // D3D resources
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;

    // GDI resources for PrintWindow capture
    HDC memDC_ = nullptr;
    HBITMAP memBitmap_ = nullptr;
    HBITMAP oldBitmap_ = nullptr;
    void* dibBits_ = nullptr;  // Direct pointer to DIB section pixel data
    int bitmapWidth_ = 0;   // full window rect (DIB) dimensions
    int bitmapHeight_ = 0;

    CaptureConfig config_;
    uint32_t width_ = 0;    // visible (cropped) output dimensions, even
    uint32_t height_ = 0;
    int cropOffsetX_ = 0;   // invisible border offset inside the DIB
    int cropOffsetY_ = 0;

    std::thread captureThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> capturing_{false};

    mutable std::mutex callbackMutex_;
    FrameCallback frameCallback_;
    ErrorCallback errorCallback_;

    int64_t qpcFrequency_ = 0;
};

} // namespace pb
