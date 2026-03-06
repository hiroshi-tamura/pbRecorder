#include "capture/DxgiScreenCapture.h"

#include <dxgi.h>
#include <dxgi1_2.h>
#include <d3d11.h>

#include <algorithm>
#include <cstring>
#include <chrono>

namespace pb {

DxgiScreenCapture::DxgiScreenCapture() {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    qpcFrequency_ = freq.QuadPart;
}

DxgiScreenCapture::~DxgiScreenCapture() {
    stop();
}

bool DxgiScreenCapture::initialize(const CaptureConfig& config, ID3D11Device* device) {
    if (!device) {
        reportError("DxgiScreenCapture::initialize: device is null");
        return false;
    }

    config_ = config;
    device_ = device;
    device_->GetImmediateContext(&context_);

    // Find the adapter and output for the requested monitor
    ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = device_->QueryInterface(__uuidof(IDXGIDevice),
                                          reinterpret_cast<void**>(dxgiDevice.GetAddressOf()));
    if (FAILED(hr)) {
        reportError("DxgiScreenCapture: QueryInterface IDXGIDevice failed: " + hrToString(hr));
        return false;
    }

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr)) {
        reportError("DxgiScreenCapture: GetAdapter failed: " + hrToString(hr));
        return false;
    }

    ComPtr<IDXGIOutput> output;
    hr = adapter->EnumOutputs(config_.monitorIndex, &output);
    if (FAILED(hr)) {
        reportError("DxgiScreenCapture: EnumOutputs failed for monitor "
                     + std::to_string(config_.monitorIndex) + ": " + hrToString(hr));
        return false;
    }

    DXGI_OUTPUT_DESC outputDesc;
    output->GetDesc(&outputDesc);
    width_ = static_cast<uint32_t>(outputDesc.DesktopCoordinates.right -
                                    outputDesc.DesktopCoordinates.left);
    height_ = static_cast<uint32_t>(outputDesc.DesktopCoordinates.bottom -
                                     outputDesc.DesktopCoordinates.top);

    hr = output.As(&output_);
    if (FAILED(hr)) {
        reportError("DxgiScreenCapture: QueryInterface IDXGIOutput1 failed: " + hrToString(hr));
        return false;
    }

    if (!createDuplication()) {
        return false;
    }

    return true;
}

bool DxgiScreenCapture::createDuplication() {
    releaseDuplication();

    HRESULT hr = output_->DuplicateOutput(device_.Get(), &duplication_);
    if (FAILED(hr)) {
        reportError("DxgiScreenCapture: DuplicateOutput failed: " + hrToString(hr));
        return false;
    }

    // Reset cursor state on new duplication
    cursorShapeValid_ = false;
    cursorShapeBuffer_.clear();
    std::memset(&cursorShapeInfo_, 0, sizeof(cursorShapeInfo_));

    return true;
}

void DxgiScreenCapture::releaseDuplication() {
    duplication_.Reset();
}

bool DxgiScreenCapture::start() {
    if (running_.load()) return true;
    if (!duplication_ && !createDuplication()) return false;

    running_ = true;
    capturing_ = true;

    captureThread_ = std::thread([this]() { captureLoop(); });
    return true;
}

bool DxgiScreenCapture::stop() {
    if (!running_.load()) return true;

    running_ = false;
    capturing_ = false;

    if (captureThread_.joinable()) {
        captureThread_.join();
    }

    releaseDuplication();
    stagingTexture_.Reset();

    return true;
}

void DxgiScreenCapture::setFrameCallback(FrameCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    frameCallback_ = std::move(callback);
}

void DxgiScreenCapture::setErrorCallback(ErrorCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    errorCallback_ = std::move(callback);
}

uint32_t DxgiScreenCapture::getWidth() const { return width_; }
uint32_t DxgiScreenCapture::getHeight() const { return height_; }
bool DxgiScreenCapture::isCapturing() const { return capturing_.load(); }

void DxgiScreenCapture::captureLoop() {
    // Calculate frame interval from target FPS (default 60)
    const int targetFps = (config_.region.width > 0) ? 60 : 60; // overridden externally via video settings
    const auto frameInterval = std::chrono::microseconds(1000000 / 60);

    while (running_.load()) {
        auto frameStart = std::chrono::steady_clock::now();

        ComPtr<ID3D11Texture2D> acquiredTexture;
        DXGI_OUTDUPL_FRAME_INFO frameInfo = {};

        if (acquireFrame(acquiredTexture, frameInfo)) {
            // Copy to our own texture so the acquired resource can be released
            ComPtr<ID3D11Texture2D> frameCopy = copyTexture(acquiredTexture.Get());

            // Release acquired frame immediately
            duplication_->ReleaseFrame();

            if (frameCopy) {
                // Draw cursor if requested
                if (config_.captureCursor) {
                    compositeCursor(frameCopy.Get(), frameInfo);
                }

                VideoFrame vf;
                vf.texture = frameCopy;
                vf.timestamp = queryTimestamp();
                vf.width = width_;
                vf.height = height_;

                std::lock_guard<std::mutex> lock(callbackMutex_);
                if (frameCallback_) {
                    frameCallback_(vf);
                }
            }
        }

        // Sleep to maintain target FPS
        auto frameEnd = std::chrono::steady_clock::now();
        auto elapsed = frameEnd - frameStart;
        if (elapsed < frameInterval) {
            std::this_thread::sleep_for(frameInterval - elapsed);
        }
    }
}

bool DxgiScreenCapture::acquireFrame(ComPtr<ID3D11Texture2D>& outTexture,
                                      DXGI_OUTDUPL_FRAME_INFO& frameInfo) {
    if (!duplication_) {
        if (!createDuplication()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            return false;
        }
    }

    ComPtr<IDXGIResource> resource;
    HRESULT hr = duplication_->AcquireNextFrame(16, &frameInfo, &resource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        // No new frame available; skip
        return false;
    }

    if (hr == DXGI_ERROR_ACCESS_LOST) {
        // Desktop duplication was lost (e.g., mode change, UAC, lock screen)
        releaseDuplication();
        if (!createDuplication()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return false;
    }

    if (FAILED(hr)) {
        reportError("DxgiScreenCapture: AcquireNextFrame failed: " + hrToString(hr));
        duplication_->ReleaseFrame();
        return false;
    }

    // Update cursor shape if changed
    if (frameInfo.PointerShapeBufferSize > 0) {
        cursorShapeBuffer_.resize(frameInfo.PointerShapeBufferSize);
        UINT requiredSize = 0;
        hr = duplication_->GetFramePointerShape(
            static_cast<UINT>(cursorShapeBuffer_.size()),
            cursorShapeBuffer_.data(),
            &requiredSize,
            &cursorShapeInfo_);
        if (SUCCEEDED(hr)) {
            cursorShapeValid_ = true;
        }
    }

    // Update cursor position
    if (frameInfo.LastMouseUpdateTime.QuadPart != 0) {
        lastCursorPos_ = frameInfo.PointerPosition.Position;
        lastCursorVisible_ = (frameInfo.PointerPosition.Visible != FALSE);
    }

    hr = resource.As(&outTexture);
    if (FAILED(hr)) {
        reportError("DxgiScreenCapture: QueryInterface ID3D11Texture2D failed: " + hrToString(hr));
        duplication_->ReleaseFrame();
        return false;
    }

    return true;
}

ComPtr<ID3D11Texture2D> DxgiScreenCapture::copyTexture(ID3D11Texture2D* source) {
    if (!source) return nullptr;

    D3D11_TEXTURE2D_DESC desc;
    source->GetDesc(&desc);

    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> copy;
    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &copy);
    if (FAILED(hr)) {
        reportError("DxgiScreenCapture: CreateTexture2D (copy) failed: " + hrToString(hr));
        return nullptr;
    }

    context_->CopyResource(copy.Get(), source);
    return copy;
}

void DxgiScreenCapture::compositeCursor(ID3D11Texture2D* texture,
                                         const DXGI_OUTDUPL_FRAME_INFO& /*frameInfo*/) {
    if (!cursorShapeValid_ || !lastCursorVisible_) return;
    if (cursorShapeBuffer_.empty()) return;

    D3D11_TEXTURE2D_DESC texDesc;
    texture->GetDesc(&texDesc);

    // Create or re-create staging texture for CPU access
    if (!stagingTexture_) {
        D3D11_TEXTURE2D_DESC stagingDesc = {};
        stagingDesc.Width = texDesc.Width;
        stagingDesc.Height = texDesc.Height;
        stagingDesc.MipLevels = 1;
        stagingDesc.ArraySize = 1;
        stagingDesc.Format = texDesc.Format;
        stagingDesc.SampleDesc.Count = 1;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;

        HRESULT hr = device_->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture_);
        if (FAILED(hr)) return;
    }

    // Copy frame to staging
    context_->CopyResource(stagingTexture_.Get(), texture);

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = context_->Map(stagingTexture_.Get(), 0, D3D11_MAP_READ_WRITE, 0, &mapped);
    if (FAILED(hr)) return;

    // Draw cursor onto the mapped surface
    const int cursorX = lastCursorPos_.x;
    const int cursorY = lastCursorPos_.y;
    const int texW = static_cast<int>(texDesc.Width);
    const int texH = static_cast<int>(texDesc.Height);

    uint8_t* destPixels = static_cast<uint8_t*>(mapped.pData);
    const uint32_t destPitch = mapped.RowPitch;

    if (cursorShapeInfo_.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
        // Monochrome cursor: top half is AND mask, bottom half is XOR mask
        const int cw = static_cast<int>(cursorShapeInfo_.Width);
        const int ch = static_cast<int>(cursorShapeInfo_.Height) / 2;
        const int pitch = static_cast<int>(cursorShapeInfo_.Pitch);

        for (int row = 0; row < ch; ++row) {
            for (int col = 0; col < cw; ++col) {
                int px = cursorX + col;
                int py = cursorY + row;
                if (px < 0 || px >= texW || py < 0 || py >= texH) continue;

                int byteIdx = col / 8;
                int bitIdx = 7 - (col % 8);

                uint8_t andBit = (cursorShapeBuffer_[row * pitch + byteIdx] >> bitIdx) & 1;
                uint8_t xorBit = (cursorShapeBuffer_[(row + ch) * pitch + byteIdx] >> bitIdx) & 1;

                uint8_t* pixel = destPixels + py * destPitch + px * 4;

                if (andBit == 0 && xorBit == 0) {
                    // Black (opaque)
                    pixel[0] = 0;   pixel[1] = 0;   pixel[2] = 0;   pixel[3] = 255;
                } else if (andBit == 0 && xorBit == 1) {
                    // White (opaque)
                    pixel[0] = 255; pixel[1] = 255; pixel[2] = 255; pixel[3] = 255;
                } else if (andBit == 1 && xorBit == 1) {
                    // XOR (invert)
                    pixel[0] ^= 0xFF;
                    pixel[1] ^= 0xFF;
                    pixel[2] ^= 0xFF;
                }
                // andBit==1, xorBit==0 => transparent, leave pixel unchanged
            }
        }
    } else if (cursorShapeInfo_.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
        // 32-bit BGRA color cursor
        const int cw = static_cast<int>(cursorShapeInfo_.Width);
        const int ch = static_cast<int>(cursorShapeInfo_.Height);
        const int pitch = static_cast<int>(cursorShapeInfo_.Pitch);

        for (int row = 0; row < ch; ++row) {
            for (int col = 0; col < cw; ++col) {
                int px = cursorX + col;
                int py = cursorY + row;
                if (px < 0 || px >= texW || py < 0 || py >= texH) continue;

                const uint8_t* src = cursorShapeBuffer_.data() + row * pitch + col * 4;
                uint8_t alpha = src[3];
                if (alpha == 0) continue;

                uint8_t* dst = destPixels + py * destPitch + px * 4;

                if (alpha == 255) {
                    dst[0] = src[0]; // B
                    dst[1] = src[1]; // G
                    dst[2] = src[2]; // R
                    dst[3] = 255;
                } else {
                    // Alpha blend
                    float a = alpha / 255.0f;
                    float inv = 1.0f - a;
                    dst[0] = static_cast<uint8_t>(src[0] * a + dst[0] * inv);
                    dst[1] = static_cast<uint8_t>(src[1] * a + dst[1] * inv);
                    dst[2] = static_cast<uint8_t>(src[2] * a + dst[2] * inv);
                    dst[3] = 255;
                }
            }
        }
    } else if (cursorShapeInfo_.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
        // Masked color: if alpha bit set, XOR with screen; otherwise overwrite
        const int cw = static_cast<int>(cursorShapeInfo_.Width);
        const int ch = static_cast<int>(cursorShapeInfo_.Height);
        const int pitch = static_cast<int>(cursorShapeInfo_.Pitch);

        for (int row = 0; row < ch; ++row) {
            for (int col = 0; col < cw; ++col) {
                int px = cursorX + col;
                int py = cursorY + row;
                if (px < 0 || px >= texW || py < 0 || py >= texH) continue;

                const uint8_t* src = cursorShapeBuffer_.data() + row * pitch + col * 4;
                uint8_t* dst = destPixels + py * destPitch + px * 4;

                if (src[3]) {
                    // XOR
                    dst[0] ^= src[0];
                    dst[1] ^= src[1];
                    dst[2] ^= src[2];
                } else {
                    // Replace
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                    dst[3] = 255;
                }
            }
        }
    }

    context_->Unmap(stagingTexture_.Get(), 0);

    // Copy back from staging to the output texture
    context_->CopyResource(texture, stagingTexture_.Get());
}

int64_t DxgiScreenCapture::queryTimestamp() const {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    // Convert to 100ns units (Media Foundation time units)
    // timestamp = counter * 10,000,000 / frequency
    return static_cast<int64_t>(
        (static_cast<double>(counter.QuadPart) / static_cast<double>(qpcFrequency_)) * 10000000.0
    );
}

void DxgiScreenCapture::reportError(const std::string& msg) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (errorCallback_) {
        errorCallback_(msg);
    }
}

} // namespace pb
