#pragma once

#include "capture/ICaptureSource.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <thread>
#include <atomic>
#include <mutex>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace pb {

class DxgiScreenCapture : public ICaptureSource {
public:
    DxgiScreenCapture();
    ~DxgiScreenCapture() override;

    bool initialize(const CaptureConfig& config, ID3D11Device* device) override;
    bool start() override;
    bool stop() override;
    void setFrameCallback(FrameCallback callback) override;
    void setErrorCallback(ErrorCallback callback) override;
    uint32_t getWidth() const override;
    uint32_t getHeight() const override;
    bool isCapturing() const override;

private:
    bool createDuplication();
    void releaseDuplication();
    void captureLoop();
    bool acquireFrame(ComPtr<ID3D11Texture2D>& outTexture,
                      DXGI_OUTDUPL_FRAME_INFO& frameInfo);
    ComPtr<ID3D11Texture2D> copyTexture(ID3D11Texture2D* source);
    void compositeCursor(ID3D11Texture2D* texture,
                         const DXGI_OUTDUPL_FRAME_INFO& frameInfo);
    int64_t queryTimestamp() const;
    void reportError(const std::string& msg);

    // D3D resources (device is NOT owned, just referenced)
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutput1> output_;
    ComPtr<IDXGIOutputDuplication> duplication_;

    // Cursor state
    std::vector<uint8_t> cursorShapeBuffer_;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO cursorShapeInfo_ = {};
    bool cursorShapeValid_ = false;
    POINT lastCursorPos_ = {};
    bool lastCursorVisible_ = false;

    // Staging texture for cursor compositing
    ComPtr<ID3D11Texture2D> stagingTexture_;

    CaptureConfig config_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;

    std::thread captureThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> capturing_{false};

    mutable std::mutex callbackMutex_;
    FrameCallback frameCallback_;
    ErrorCallback errorCallback_;

    // QPC frequency for timestamp conversion
    int64_t qpcFrequency_ = 0;
};

} // namespace pb
