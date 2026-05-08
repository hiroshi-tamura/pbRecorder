#include "capture/UiElementCapture.h"

#include <windows.h>
#include <dxgi.h>
#include <dwmapi.h>

#include <algorithm>

namespace pb {

namespace {

bool validRegion(const RegionRect& region)
{
    return region.width > 1 && region.height > 1;
}

} // namespace

UiElementCapture::UiElementCapture() = default;

UiElementCapture::~UiElementCapture()
{
    stop();
}

bool UiElementCapture::initialize(const CaptureConfig& config, ID3D11Device* device)
{
    if (!device) {
        reportError("UiElementCapture::initialize: device is null");
        return false;
    }

    config_ = config;
    target_ = config.uiElement;
    captureFromWindow_ = config.uiElementCaptureFromWindow;
    if (captureFromWindow_ && (!target_.rootWindow || !IsWindow(target_.rootWindow))) {
        reportError("UiElementCapture: parent window capture is enabled, but the selected parent window is no longer available");
        return false;
    }
    if (!validRegion(target_.initialRect)) {
        reportError("UiElementCapture::initialize: invalid UI element bounds");
        return false;
    }

    device_ = device;
    device_->GetImmediateContext(&context_);

    lastRect_ = target_.initialRect;
    if (!validRegion(lastRect_)) {
        reportError("UiElementCapture::initialize: could not resolve UI element bounds");
        return false;
    }

    width_ = static_cast<uint32_t>(target_.initialRect.width) & ~1u;
    height_ = static_cast<uint32_t>(target_.initialRect.height) & ~1u;
    if (width_ == 0 || height_ == 0) {
        reportError("UiElementCapture::initialize: UI element bounds are too small");
        return false;
    }

    CaptureConfig innerConfig = config;
    if (captureFromWindow_) {
        RegionRect rootRect{};
        if (!rootWindowRect(rootRect)) {
            reportError("UiElementCapture: failed to resolve parent window bounds");
            return false;
        }
        monitorX_ = rootRect.x;
        monitorY_ = rootRect.y;
        innerConfig.mode = CaptureMode::Window;
        innerConfig.targetWindow = target_.rootWindow;
        innerCapture_ = std::make_unique<WindowCapture>();
    } else {
        monitorIndex_ = findBestMonitor(lastRect_);
        if (!monitorOrigin(monitorIndex_, monitorX_, monitorY_)) {
            monitorX_ = 0;
            monitorY_ = 0;
        }
        innerConfig.mode = CaptureMode::Screen;
        innerConfig.monitorIndex = monitorIndex_;
        innerCapture_ = std::make_unique<DxgiScreenCapture>();
    }
    innerConfig.captureCursor = config.captureCursor;

    if (!innerCapture_->initialize(innerConfig, device)) {
        reportError("UiElementCapture: failed to initialize inner capture");
        return false;
    }

    screenWidth_ = innerCapture_->getWidth();
    screenHeight_ = innerCapture_->getHeight();

    if (!recreateCropTexture()) {
        return false;
    }

    innerCapture_->setFrameCallback(
        [this](const VideoFrame& frame) { onFullFrame(frame); });
    innerCapture_->setErrorCallback(
        [this](const std::string& err) { handleInnerError(err); });

    return true;
}

bool UiElementCapture::start()
{
    if (capturing_.load()) return true;
    if (!innerCapture_) return false;

    capturing_ = true;
    if (innerCapture_->start()) {
        return true;
    }

    capturing_ = false;
    return false;
}

bool UiElementCapture::stop()
{
    if (!capturing_.load()) return true;

    capturing_ = false;
    if (innerCapture_) {
        innerCapture_->stop();
    }
    cropRtv_.Reset();
    cropTexture_.Reset();
    return true;
}

void UiElementCapture::handleInnerError(const std::string& err)
{
    reportError("UiElementCapture(inner): " + err);
}

void UiElementCapture::setFrameCallback(FrameCallback callback)
{
    std::lock_guard<std::mutex> lock(callbackMutex_);
    frameCallback_ = std::move(callback);
}

void UiElementCapture::setErrorCallback(ErrorCallback callback)
{
    std::lock_guard<std::mutex> lock(callbackMutex_);
    errorCallback_ = std::move(callback);
}

uint32_t UiElementCapture::getWidth() const { return width_; }
uint32_t UiElementCapture::getHeight() const { return height_; }
bool UiElementCapture::isCapturing() const { return capturing_.load(); }

int UiElementCapture::findBestMonitor(const RegionRect& region) const
{
    ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = device_->QueryInterface(__uuidof(IDXGIDevice),
                                         reinterpret_cast<void**>(dxgiDevice.GetAddressOf()));
    if (FAILED(hr)) return 0;

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr)) return 0;

    RECT regionRect{
        region.x,
        region.y,
        region.x + region.width,
        region.y + region.height
    };

    int bestIdx = 0;
    int64_t bestArea = 0;
    ComPtr<IDXGIOutput> output;
    for (UINT i = 0; adapter->EnumOutputs(i, &output) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_OUTPUT_DESC desc{};
        output->GetDesc(&desc);
        const RECT& mr = desc.DesktopCoordinates;

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
        output.Reset();
    }
    return bestIdx;
}

bool UiElementCapture::monitorOrigin(int monitorIndex, int& x, int& y) const
{
    ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = device_->QueryInterface(__uuidof(IDXGIDevice),
                                         reinterpret_cast<void**>(dxgiDevice.GetAddressOf()));
    if (FAILED(hr)) return false;

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIOutput> output;
    hr = adapter->EnumOutputs(static_cast<UINT>(monitorIndex), &output);
    if (FAILED(hr)) return false;

    DXGI_OUTPUT_DESC desc{};
    output->GetDesc(&desc);
    x = desc.DesktopCoordinates.left;
    y = desc.DesktopCoordinates.top;
    return true;
}

bool UiElementCapture::rootWindowRect(RegionRect& rect) const
{
    if (!target_.rootWindow || !IsWindow(target_.rootWindow)) {
        return false;
    }

    RECT wr{};
    HRESULT hr = DwmGetWindowAttribute(target_.rootWindow,
                                       DWMWA_EXTENDED_FRAME_BOUNDS,
                                       &wr,
                                       sizeof(wr));
    if (FAILED(hr) && !GetWindowRect(target_.rootWindow, &wr)) {
        return false;
    }

    rect = {
        wr.left,
        wr.top,
        wr.right - wr.left,
        wr.bottom - wr.top
    };
    return rect.width > 0 && rect.height > 0;
}

void UiElementCapture::onFullFrame(const VideoFrame& frame)
{
    if (!capturing_.load()) return;

    auto resolved = uiAutomation_.resolveRect(target_);
    if (resolved && validRegion(*resolved)) {
        RegionRect next = *resolved;
        if (captureFromWindow_) {
            lastRect_ = next;
            RegionRect rootRect{};
            if (rootWindowRect(rootRect)) {
                monitorX_ = rootRect.x;
                monitorY_ = rootRect.y;
            }
        } else {
            int nextMonitor = findBestMonitor(next);
            if (nextMonitor == monitorIndex_) {
                lastRect_ = next;
            }
        }
    }

    if (!cropTexture_) {
        if (!recreateCropTexture()) {
            return;
        }
    }

    if (cropRtv_) {
        const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        context_->ClearRenderTargetView(cropRtv_.Get(), black);
    }

    int srcX = lastRect_.x - monitorX_;
    int srcY = lastRect_.y - monitorY_;
    int srcW = std::min<int>(lastRect_.width, static_cast<int>(width_));
    int srcH = std::min<int>(lastRect_.height, static_cast<int>(height_));

    srcX = std::clamp(srcX, 0, static_cast<int>(frame.width));
    srcY = std::clamp(srcY, 0, static_cast<int>(frame.height));
    if (srcX >= static_cast<int>(frame.width) || srcY >= static_cast<int>(frame.height)) {
        return;
    }

    srcW = std::min(srcW, static_cast<int>(frame.width) - srcX);
    srcH = std::min(srcH, static_cast<int>(frame.height) - srcY);
    if (srcW <= 0 || srcH <= 0) {
        return;
    }

    D3D11_BOX srcBox{};
    srcBox.left = static_cast<UINT>(srcX);
    srcBox.top = static_cast<UINT>(srcY);
    srcBox.right = static_cast<UINT>(srcX + srcW);
    srcBox.bottom = static_cast<UINT>(srcY + srcH);
    srcBox.front = 0;
    srcBox.back = 1;

    context_->CopySubresourceRegion(
        cropTexture_.Get(), 0,
        0, 0, 0,
        frame.texture.Get(), 0,
        &srcBox);

    D3D11_TEXTURE2D_DESC cropDesc{};
    cropTexture_->GetDesc(&cropDesc);

    ComPtr<ID3D11Texture2D> outputTexture;
    HRESULT hr = device_->CreateTexture2D(&cropDesc, nullptr, &outputTexture);
    if (FAILED(hr)) {
        reportError("UiElementCapture: failed to create output texture: " + hrToString(hr));
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

bool UiElementCapture::recreateCropTexture()
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    cropRtv_.Reset();
    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &cropTexture_);
    if (FAILED(hr)) {
        reportError("UiElementCapture: failed to create crop texture: " + hrToString(hr));
        return false;
    }
    hr = device_->CreateRenderTargetView(cropTexture_.Get(), nullptr, &cropRtv_);
    if (FAILED(hr)) {
        reportError("UiElementCapture: failed to create crop render target: " + hrToString(hr));
        return false;
    }
    return true;
}

void UiElementCapture::reportError(const std::string& msg)
{
    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (errorCallback_) {
        errorCallback_(msg);
    }
}

} // namespace pb
