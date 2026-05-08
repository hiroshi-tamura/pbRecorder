#pragma once

#include "capture/ICaptureSource.h"
#include "capture/DxgiScreenCapture.h"
#include "capture/WindowCapture.h"
#include "core/UiAutomationHelper.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <memory>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace pb {

class UiElementCapture : public ICaptureSource {
public:
    UiElementCapture();
    ~UiElementCapture() override;

    bool initialize(const CaptureConfig& config, ID3D11Device* device) override;
    bool start() override;
    bool stop() override;
    void setFrameCallback(FrameCallback callback) override;
    void setErrorCallback(ErrorCallback callback) override;
    uint32_t getWidth() const override;
    uint32_t getHeight() const override;
    bool isCapturing() const override;

private:
    int findBestMonitor(const RegionRect& region) const;
    bool monitorOrigin(int monitorIndex, int& x, int& y) const;
    bool rootWindowRect(RegionRect& rect) const;
    void onFullFrame(const VideoFrame& frame);
    bool recreateCropTexture();
    void handleInnerError(const std::string& err);
    void reportError(const std::string& msg);

    std::unique_ptr<ICaptureSource> innerCapture_;
    UiAutomationHelper uiAutomation_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11Texture2D> cropTexture_;
    ComPtr<ID3D11RenderTargetView> cropRtv_;

    CaptureConfig config_;
    UiElementTarget target_;
    RegionRect lastRect_ = {};
    int monitorIndex_ = 0;
    int monitorX_ = 0;
    int monitorY_ = 0;
    bool captureFromWindow_ = true;
    uint32_t screenWidth_ = 0;
    uint32_t screenHeight_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;

    std::atomic<bool> capturing_{false};

    mutable std::mutex callbackMutex_;
    FrameCallback frameCallback_;
    ErrorCallback errorCallback_;
};

} // namespace pb
