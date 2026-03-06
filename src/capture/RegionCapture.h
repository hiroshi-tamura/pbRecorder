#pragma once

#include "capture/ICaptureSource.h"
#include "capture/DxgiScreenCapture.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <thread>
#include <atomic>
#include <mutex>
#include <memory>

using Microsoft::WRL::ComPtr;

namespace pb {

class RegionCapture : public ICaptureSource {
public:
    RegionCapture();
    ~RegionCapture() override;

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
    void onFullFrame(const VideoFrame& frame);
    void reportError(const std::string& msg);

    std::unique_ptr<DxgiScreenCapture> screenCapture_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;

    // Crop texture (reused if dimensions match)
    ComPtr<ID3D11Texture2D> cropTexture_;

    CaptureConfig config_;
    RegionRect region_ = {};
    uint32_t width_ = 0;
    uint32_t height_ = 0;

    // Offset within the captured monitor
    int offsetX_ = 0;
    int offsetY_ = 0;

    // Full screen dimensions from the inner capture
    uint32_t screenWidth_ = 0;
    uint32_t screenHeight_ = 0;

    std::atomic<bool> capturing_{false};

    mutable std::mutex callbackMutex_;
    FrameCallback frameCallback_;
    ErrorCallback errorCallback_;
};

} // namespace pb
