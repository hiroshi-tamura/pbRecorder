#include "capture/WindowCapture.h"

#include <dwmapi.h>
#include <wingdi.h>

#include <algorithm>
#include <cstring>
#include <chrono>
#include <exception>
#include <string>

// Link: dwmapi.lib is needed at link time

namespace pb {

WindowCapture::WindowCapture() {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    qpcFrequency_ = freq.QuadPart;
}

WindowCapture::~WindowCapture() {
    stop();
    releaseGdiResources();
}

bool WindowCapture::initialize(const CaptureConfig& config, ID3D11Device* device) {
    if (!device) {
        reportError("WindowCapture::initialize: device is null");
        return false;
    }
    if (!config.targetWindow || !IsWindow(config.targetWindow)) {
        reportError("WindowCapture::initialize: invalid target window handle");
        return false;
    }

    config_ = config;
    device_ = device;
    device_->GetImmediateContext(&context_);

    // Determine the full window rect (PrintWindow's coordinate space) and the
    // visible content sub-rectangle. The DIB is allocated at the full window
    // size; PrintWindow draws into it, and we crop the visible region.
    RECT fullRect;
    int cropW = 0, cropH = 0, offX = 0, offY = 0;
    if (!getCaptureGeometry(fullRect, cropW, cropH, offX, offY)) {
        reportError("WindowCapture::initialize: could not get window rect");
        return false;
    }

    if (cropW <= 0 || cropH <= 0) {
        reportError("WindowCapture::initialize: window has zero dimensions");
        return false;
    }

    // H.264 encoders require even dimensions
    width_ = static_cast<uint32_t>(cropW & ~1);
    height_ = static_cast<uint32_t>(cropH & ~1);
    cropOffsetX_ = offX;
    cropOffsetY_ = offY;

    int fullW = static_cast<int>(fullRect.right - fullRect.left);
    int fullH = static_cast<int>(fullRect.bottom - fullRect.top);
    if (!createGdiResources(fullW, fullH)) {
        return false;
    }

    return true;
}

bool WindowCapture::start() {
    if (running_.load()) return true;

    if (!isWindowValid()) {
        reportError("WindowCapture::start: target window is no longer valid");
        return false;
    }

    running_ = true;
    capturing_ = true;

    captureThread_ = std::thread([this]() { captureLoop(); });
    return true;
}

bool WindowCapture::stop() {
    if (!running_.load()) return true;

    running_ = false;
    capturing_ = false;

    if (captureThread_.joinable()) {
        captureThread_.join();
    }

    return true;
}

void WindowCapture::setFrameCallback(FrameCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    frameCallback_ = std::move(callback);
}

void WindowCapture::setErrorCallback(ErrorCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    errorCallback_ = std::move(callback);
}

uint32_t WindowCapture::getWidth() const { return width_; }
uint32_t WindowCapture::getHeight() const { return height_; }
bool WindowCapture::isCapturing() const { return capturing_.load(); }

void WindowCapture::captureLoop() {
    const int targetFps = std::clamp(config_.targetFps, 1, 240);
    const auto frameInterval = std::chrono::microseconds(1000000 / targetFps);

    while (running_.load()) {
        auto frameStart = std::chrono::steady_clock::now();

        if (!isWindowValid()) {
            reportError("WindowCapture: target window was closed");
            capturing_ = false;
            running_ = false;
            break;
        }

        captureFrame();

        auto frameEnd = std::chrono::steady_clock::now();
        auto elapsed = frameEnd - frameStart;
        if (elapsed < frameInterval) {
            std::this_thread::sleep_for(frameInterval - elapsed);
        }
    }
}

bool WindowCapture::captureFrame() {
    // Get current geometry (window may have been resized/moved)
    RECT fullRect;
    int cropW = 0, cropH = 0, offX = 0, offY = 0;
    if (!getCaptureGeometry(fullRect, cropW, cropH, offX, offY)) return false;

    int fullW = fullRect.right - fullRect.left;
    int fullH = fullRect.bottom - fullRect.top;
    if (fullW <= 0 || fullH <= 0) return false;

    // Visible output dimensions, even for H.264
    int outW = cropW & ~1;
    int outH = cropH & ~1;
    if (outW <= 0 || outH <= 0) return false;

    // Clamp crop window to the DIB bounds
    if (offX < 0) offX = 0;
    if (offY < 0) offY = 0;
    if (offX + outW > fullW) outW = (fullW - offX) & ~1;
    if (offY + outH > fullH) outH = (fullH - offY) & ~1;
    if (outW <= 0 || outH <= 0) return false;

    // Handle window resize (reallocate the full-window-sized DIB)
    if (fullW != bitmapWidth_ || fullH != bitmapHeight_) {
        releaseGdiResources();
        if (!createGdiResources(fullW, fullH)) return false;
    }
    width_ = static_cast<uint32_t>(outW);
    height_ = static_cast<uint32_t>(outH);
    cropOffsetX_ = offX;
    cropOffsetY_ = offY;

    // Use PrintWindow with PW_RENDERFULLCONTENT for DWM-compatible capture.
    // PrintWindow renders the target window's own content directly, so any
    // other window overlapping it on screen is NOT captured.
    // PW_RENDERFULLCONTENT = 0x00000002
    static constexpr UINT PW_RENDERFULLCONTENT_FLAG = 0x00000002;

    BOOL ok = PrintWindow(config_.targetWindow, memDC_, PW_RENDERFULLCONTENT_FLAG);
    if (!ok) {
        reportError("WindowCapture: PrintWindow failed");
        return false;
    }

    // Composite cursor if needed (relative to the full window rect)
    if (config_.captureCursor) {
        compositeCursor(memDC_, fullRect);
    }

    // Convert bitmap to D3D11 texture, cropping out the invisible border offset
    ComPtr<ID3D11Texture2D> texture = bitmapToTexture(offX, offY, outW, outH);
    if (!texture) return false;

    VideoFrame vf;
    vf.texture = texture;
    vf.timestamp = queryTimestamp();
    vf.width = static_cast<uint32_t>(outW);
    vf.height = static_cast<uint32_t>(outH);

    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (frameCallback_) {
        try {
            frameCallback_(vf);
        } catch (const std::exception& e) {
            reportError(std::string("WindowCapture callback failed: ") + e.what());
            capturing_.store(false);
        } catch (...) {
            reportError("WindowCapture callback failed with an unknown error");
            capturing_.store(false);
        }
    }

    return true;
}

bool WindowCapture::createGdiResources(int width, int height) {
    releaseGdiResources();

    HDC screenDC = GetDC(nullptr);
    if (!screenDC) {
        reportError("WindowCapture: GetDC(nullptr) failed");
        return false;
    }

    memDC_ = CreateCompatibleDC(screenDC);
    if (!memDC_) {
        ReleaseDC(nullptr, screenDC);
        reportError("WindowCapture: CreateCompatibleDC failed");
        return false;
    }

    // Create a 32-bit DIB section so we can read pixel data directly
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    dibBits_ = nullptr;
    memBitmap_ = CreateDIBSection(memDC_, &bmi, DIB_RGB_COLORS, &dibBits_, nullptr, 0);
    if (!memBitmap_ || !dibBits_) {
        DeleteDC(memDC_);
        memDC_ = nullptr;
        ReleaseDC(nullptr, screenDC);
        reportError("WindowCapture: CreateDIBSection failed");
        return false;
    }

    oldBitmap_ = static_cast<HBITMAP>(SelectObject(memDC_, memBitmap_));
    ReleaseDC(nullptr, screenDC);

    bitmapWidth_ = width;
    bitmapHeight_ = height;

    return true;
}

void WindowCapture::releaseGdiResources() {
    if (memDC_) {
        if (oldBitmap_) {
            SelectObject(memDC_, oldBitmap_);
            oldBitmap_ = nullptr;
        }
        DeleteDC(memDC_);
        memDC_ = nullptr;
    }
    if (memBitmap_) {
        DeleteObject(memBitmap_);
        memBitmap_ = nullptr;
    }
    dibBits_ = nullptr;
    bitmapWidth_ = 0;
    bitmapHeight_ = 0;
}

ComPtr<ID3D11Texture2D> WindowCapture::bitmapToTexture(int offsetX, int offsetY,
                                                        int cropW, int cropH) {
    if (!dibBits_) {
        reportError("WindowCapture: DIB section bits pointer is null");
        return nullptr;
    }

    // GDI must flush any pending operations to ensure the DIB bits are up-to-date
    GdiFlush();

    // DIB section data is directly accessible via dibBits_.
    // GDI gives us BGRX (alpha=0). Set alpha to 255 over the whole DIB.
    const size_t fullPixelCount =
        static_cast<size_t>(bitmapWidth_) * static_cast<size_t>(bitmapHeight_);
    uint8_t* data = static_cast<uint8_t*>(dibBits_);
    for (size_t i = 0; i < fullPixelCount; ++i) {
        data[i * 4 + 3] = 255; // set alpha
    }

    const size_t fullPitch = static_cast<size_t>(bitmapWidth_) * 4;

    // Create D3D11 texture from the cropped sub-region of the DIB. We keep the
    // full DIB pitch and point the source at the cropped top-left so the
    // invisible border offset is removed without copying.
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(cropW);
    desc.Height = static_cast<UINT>(cropH);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = data + static_cast<size_t>(offsetY) * fullPitch +
                       static_cast<size_t>(offsetX) * 4;
    initData.SysMemPitch = static_cast<UINT>(fullPitch);

    ComPtr<ID3D11Texture2D> texture;
    HRESULT hr = device_->CreateTexture2D(&desc, &initData, &texture);
    if (FAILED(hr)) {
        reportError("WindowCapture: CreateTexture2D failed: " + hrToString(hr));
        return nullptr;
    }

    return texture;
}

void WindowCapture::compositeCursor(HDC hdc, const RECT& windowRect) {
    CURSORINFO ci = {};
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci)) return;
    if (!(ci.flags & CURSOR_SHOWING)) return;

    // Convert screen cursor position to window-relative coordinates
    int cursorX = ci.ptScreenPos.x - windowRect.left;
    int cursorY = ci.ptScreenPos.y - windowRect.top;

    // Check if cursor is within the window bounds
    if (cursorX < -64 || cursorX >= bitmapWidth_ + 64 ||
        cursorY < -64 || cursorY >= bitmapHeight_ + 64) {
        return;
    }

    // Use DrawIconEx to draw the cursor with proper alpha
    DrawIconEx(hdc, cursorX, cursorY, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
}

bool WindowCapture::isWindowValid() const {
    return config_.targetWindow && IsWindow(config_.targetWindow);
}

bool WindowCapture::getWindowRect(RECT& rect) const {
    // Use DWM extended frame bounds for accurate rect (excludes invisible borders)
    HRESULT hr = DwmGetWindowAttribute(config_.targetWindow,
                                        DWMWA_EXTENDED_FRAME_BOUNDS,
                                        &rect, sizeof(rect));
    if (SUCCEEDED(hr)) {
        // DWM gives us screen coordinates in physical pixels.
        // Adjust for DPI: GetDpiForWindow returns the window's DPI.
        // The rect from DWM is already in physical pixels, so no adjustment needed
        // for the dimensions. However, for PrintWindow we use the client area size.

        // For PrintWindow, we need the window size as seen by the window itself.
        // Get the actual window rect for the size PrintWindow will render to.
        RECT windowRect;
        if (GetWindowRect(config_.targetWindow, &windowRect)) {
            // Use DPI-aware sizing
            UINT dpi = 96;
            // GetDpiForWindow is available on Windows 10 1607+
            typedef UINT(WINAPI* GetDpiForWindowFunc)(HWND);
            static auto pGetDpiForWindow = reinterpret_cast<GetDpiForWindowFunc>(
                GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));

            if (pGetDpiForWindow) {
                dpi = pGetDpiForWindow(config_.targetWindow);
            }

            // DWM rect is in physical (unscaled) pixels
            // For high-DPI windows, the DWM rect already gives physical pixels.
            // We use the DWM rect directly.
            (void)dpi; // DWM rect is already in physical pixels
        }

        return true;
    }

    // Fallback to regular GetWindowRect
    return GetWindowRect(config_.targetWindow, &rect) != FALSE;
}

bool WindowCapture::getCaptureGeometry(RECT& fullRect, int& cropW, int& cropH,
                                       int& offsetX, int& offsetY) const {
    // GetWindowRect gives the coordinate space PrintWindow draws into
    // (origin = full window top-left, including invisible resize borders).
    if (!GetWindowRect(config_.targetWindow, &fullRect)) {
        return false;
    }

    // DWM extended frame bounds is the visible window rect (excludes the
    // invisible borders). The difference is the offset to crop away.
    RECT dwmRect;
    HRESULT hr = DwmGetWindowAttribute(config_.targetWindow,
                                       DWMWA_EXTENDED_FRAME_BOUNDS,
                                       &dwmRect, sizeof(dwmRect));
    if (SUCCEEDED(hr)) {
        offsetX = static_cast<int>(dwmRect.left - fullRect.left);
        offsetY = static_cast<int>(dwmRect.top - fullRect.top);
        if (offsetX < 0) offsetX = 0;
        if (offsetY < 0) offsetY = 0;
        cropW = static_cast<int>(dwmRect.right - dwmRect.left);
        cropH = static_cast<int>(dwmRect.bottom - dwmRect.top);
    } else {
        offsetX = 0;
        offsetY = 0;
        cropW = static_cast<int>(fullRect.right - fullRect.left);
        cropH = static_cast<int>(fullRect.bottom - fullRect.top);
    }
    return true;
}

int64_t WindowCapture::queryTimestamp() const {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return static_cast<int64_t>(
        (static_cast<double>(counter.QuadPart) / static_cast<double>(qpcFrequency_)) * 10000000.0
    );
}

void WindowCapture::reportError(const std::string& msg) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (errorCallback_) {
        errorCallback_(msg);
    }
}

} // namespace pb
