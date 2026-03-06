#include "capture/RegionCapture.h"

#include <windows.h>
#include <algorithm>
#include <vector>

namespace pb {

// Helper struct for monitor enumeration
struct MonitorEnumData {
    std::vector<RECT> monitorRects;
    std::vector<HMONITOR> monitorHandles;
};

static BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC /*hdc*/,
                                      LPRECT lprcMonitor, LPARAM dwData) {
    auto* data = reinterpret_cast<MonitorEnumData*>(dwData);
    data->monitorRects.push_back(*lprcMonitor);
    data->monitorHandles.push_back(hMonitor);
    return TRUE;
}

RegionCapture::RegionCapture() = default;

RegionCapture::~RegionCapture() {
    stop();
}

bool RegionCapture::initialize(const CaptureConfig& config, ID3D11Device* device) {
    if (!device) {
        reportError("RegionCapture::initialize: device is null");
        return false;
    }

    config_ = config;
    region_ = config.region;
    device_ = device;
    device_->GetImmediateContext(&context_);

    if (region_.width <= 0 || region_.height <= 0) {
        reportError("RegionCapture::initialize: invalid region dimensions");
        return false;
    }

    width_ = static_cast<uint32_t>(region_.width) & ~1u;
    height_ = static_cast<uint32_t>(region_.height) & ~1u;

    // Find the best monitor for this region
    int monitorIdx = findBestMonitor(region_);

    // Create inner DxgiScreenCapture for the selected monitor
    screenCapture_ = std::make_unique<DxgiScreenCapture>();

    CaptureConfig screenConfig = config;
    screenConfig.mode = CaptureMode::Screen;
    screenConfig.monitorIndex = monitorIdx;
    // Cursor compositing is handled at the full-screen level, so pass through the setting
    screenConfig.captureCursor = config.captureCursor;

    if (!screenCapture_->initialize(screenConfig, device)) {
        reportError("RegionCapture: failed to initialize inner screen capture");
        return false;
    }

    screenWidth_ = screenCapture_->getWidth();
    screenHeight_ = screenCapture_->getHeight();

    // Calculate offset of the region within the selected monitor
    MonitorEnumData enumData;
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&enumData));

    if (monitorIdx >= 0 && monitorIdx < static_cast<int>(enumData.monitorRects.size())) {
        const RECT& monRect = enumData.monitorRects[monitorIdx];
        offsetX_ = region_.x - monRect.left;
        offsetY_ = region_.y - monRect.top;
    } else {
        offsetX_ = region_.x;
        offsetY_ = region_.y;
    }

    // Clamp offsets to valid range
    offsetX_ = std::max(0, offsetX_);
    offsetY_ = std::max(0, offsetY_);

    // Clamp region to fit within the screen
    if (offsetX_ + static_cast<int>(width_) > static_cast<int>(screenWidth_)) {
        width_ = screenWidth_ - static_cast<uint32_t>(offsetX_);
    }
    if (offsetY_ + static_cast<int>(height_) > static_cast<int>(screenHeight_)) {
        height_ = screenHeight_ - static_cast<uint32_t>(offsetY_);
    }

    // Set up the frame callback from the inner capture
    screenCapture_->setFrameCallback(
        [this](const VideoFrame& frame) { onFullFrame(frame); });

    screenCapture_->setErrorCallback(
        [this](const std::string& err) { reportError("RegionCapture(inner): " + err); });

    return true;
}

bool RegionCapture::start() {
    if (capturing_.load()) return true;
    if (!screenCapture_) return false;

    capturing_ = true;
    return screenCapture_->start();
}

bool RegionCapture::stop() {
    if (!capturing_.load()) return true;

    capturing_ = false;

    if (screenCapture_) {
        screenCapture_->stop();
    }

    cropTexture_.Reset();
    return true;
}

void RegionCapture::setFrameCallback(FrameCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    frameCallback_ = std::move(callback);
}

void RegionCapture::setErrorCallback(ErrorCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    errorCallback_ = std::move(callback);
}

uint32_t RegionCapture::getWidth() const { return width_; }
uint32_t RegionCapture::getHeight() const { return height_; }
bool RegionCapture::isCapturing() const { return capturing_.load(); }

int RegionCapture::findBestMonitor(const RegionRect& region) const {
    MonitorEnumData enumData;
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&enumData));

    if (enumData.monitorRects.empty()) return 0;

    RECT regionRect;
    regionRect.left = region.x;
    regionRect.top = region.y;
    regionRect.right = region.x + region.width;
    regionRect.bottom = region.y + region.height;

    int bestIdx = 0;
    int64_t bestArea = 0;

    for (size_t i = 0; i < enumData.monitorRects.size(); ++i) {
        const RECT& mr = enumData.monitorRects[i];

        // Calculate intersection area
        int left = std::max(static_cast<int>(regionRect.left), static_cast<int>(mr.left));
        int top = std::max(static_cast<int>(regionRect.top), static_cast<int>(mr.top));
        int right = std::min(static_cast<int>(regionRect.right), static_cast<int>(mr.right));
        int bottom = std::min(static_cast<int>(regionRect.bottom), static_cast<int>(mr.bottom));

        if (right > left && bottom > top) {
            int64_t area = static_cast<int64_t>(right - left) * (bottom - top);
            if (area > bestArea) {
                bestArea = area;
                bestIdx = static_cast<int>(i);
            }
        }
    }

    return bestIdx;
}

void RegionCapture::onFullFrame(const VideoFrame& frame) {
    if (!capturing_.load()) return;

    // Create crop texture if needed
    if (!cropTexture_) {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width_;
        desc.Height = height_;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &cropTexture_);
        if (FAILED(hr)) {
            reportError("RegionCapture: failed to create crop texture: " + hrToString(hr));
            return;
        }
    }

    // Use CopySubresourceRegion to extract the region from the full frame
    D3D11_BOX srcBox = {};
    srcBox.left = static_cast<UINT>(offsetX_);
    srcBox.top = static_cast<UINT>(offsetY_);
    srcBox.right = static_cast<UINT>(offsetX_) + width_;
    srcBox.bottom = static_cast<UINT>(offsetY_) + height_;
    srcBox.front = 0;
    srcBox.back = 1;

    // Validate that the source box is within the source texture
    if (srcBox.right > frame.width || srcBox.bottom > frame.height) {
        // Clamp
        srcBox.right = std::min(srcBox.right, frame.width);
        srcBox.bottom = std::min(srcBox.bottom, frame.height);
        if (srcBox.right <= srcBox.left || srcBox.bottom <= srcBox.top) return;
    }

    context_->CopySubresourceRegion(
        cropTexture_.Get(), 0,
        0, 0, 0,
        frame.texture.Get(), 0,
        &srcBox);

    // Create a new texture for the output frame (so the crop texture can be reused)
    D3D11_TEXTURE2D_DESC cropDesc;
    cropTexture_->GetDesc(&cropDesc);

    ComPtr<ID3D11Texture2D> outputTexture;
    HRESULT hr = device_->CreateTexture2D(&cropDesc, nullptr, &outputTexture);
    if (FAILED(hr)) {
        reportError("RegionCapture: failed to create output texture: " + hrToString(hr));
        return;
    }
    context_->CopyResource(outputTexture.Get(), cropTexture_.Get());

    VideoFrame croppedFrame;
    croppedFrame.texture = outputTexture;
    croppedFrame.timestamp = frame.timestamp;
    croppedFrame.width = width_;
    croppedFrame.height = height_;

    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (frameCallback_) {
        frameCallback_(croppedFrame);
    }
}

void RegionCapture::reportError(const std::string& msg) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (errorCallback_) {
        errorCallback_(msg);
    }
}

} // namespace pb
