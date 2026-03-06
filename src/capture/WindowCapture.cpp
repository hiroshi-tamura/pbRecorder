#include "capture/WindowCapture.h"

#include <dwmapi.h>
#include <wingdi.h>

#include <algorithm>
#include <cstring>
#include <chrono>

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

    // Get the actual window dimensions via DWM extended frame bounds (DPI-aware)
    RECT rect;
    if (!getWindowRect(rect)) {
        reportError("WindowCapture::initialize: could not get window rect");
        return false;
    }

    width_ = static_cast<uint32_t>(rect.right - rect.left);
    height_ = static_cast<uint32_t>(rect.bottom - rect.top);

    if (width_ == 0 || height_ == 0) {
        reportError("WindowCapture::initialize: window has zero dimensions");
        return false;
    }

    // H.264 encoders require even dimensions
    width_ &= ~1u;
    height_ &= ~1u;

    if (!createGdiResources(static_cast<int>(width_), static_cast<int>(height_))) {
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
    const auto frameInterval = std::chrono::microseconds(1000000 / 60);

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
    // Get current window rect (may have been resized)
    RECT rect;
    if (!getWindowRect(rect)) return false;

    int w = rect.right - rect.left;
    int h = rect.bottom - rect.top;
    if (w <= 0 || h <= 0) return false;

    // H.264 encoders require even dimensions
    w &= ~1;
    h &= ~1;
    if (w <= 0 || h <= 0) return false;

    // Handle window resize
    if (w != bitmapWidth_ || h != bitmapHeight_) {
        releaseGdiResources();
        if (!createGdiResources(w, h)) return false;
        width_ = static_cast<uint32_t>(w);
        height_ = static_cast<uint32_t>(h);
    }

    // Use PrintWindow with PW_RENDERFULLCONTENT for DWM-compatible capture
    // PW_RENDERFULLCONTENT = 0x00000002
    static constexpr UINT PW_RENDERFULLCONTENT_FLAG = 0x00000002;

    BOOL ok = PrintWindow(config_.targetWindow, memDC_, PW_RENDERFULLCONTENT_FLAG);
    if (!ok) {
        // Fallback: try BitBlt from window DC
        HDC windowDC = GetDC(config_.targetWindow);
        if (windowDC) {
            BitBlt(memDC_, 0, 0, w, h, windowDC, 0, 0, SRCCOPY);
            ReleaseDC(config_.targetWindow, windowDC);
        } else {
            return false;
        }
    }

    // Composite cursor if needed
    if (config_.captureCursor) {
        compositeCursor(memDC_, rect);
    }

    // Convert bitmap to D3D11 texture
    ComPtr<ID3D11Texture2D> texture = bitmapToTexture(w, h);
    if (!texture) return false;

    VideoFrame vf;
    vf.texture = texture;
    vf.timestamp = queryTimestamp();
    vf.width = static_cast<uint32_t>(w);
    vf.height = static_cast<uint32_t>(h);

    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (frameCallback_) {
        frameCallback_(vf);
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

ComPtr<ID3D11Texture2D> WindowCapture::bitmapToTexture(int width, int height) {
    if (!dibBits_) {
        reportError("WindowCapture: DIB section bits pointer is null");
        return nullptr;
    }

    // GDI must flush any pending operations to ensure the DIB bits are up-to-date
    GdiFlush();

    // DIB section data is directly accessible via dibBits_.
    // GDI gives us BGRX (alpha=0). Set alpha to 255 for D3D11 compatibility.
    const size_t pixelCount = static_cast<size_t>(width) * height;
    uint8_t* data = static_cast<uint8_t*>(dibBits_);
    for (size_t i = 0; i < pixelCount; ++i) {
        data[i * 4 + 3] = 255; // set alpha
    }

    // Create D3D11 texture directly from DIB bits
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = dibBits_;
    initData.SysMemPitch = static_cast<UINT>(width * 4);

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
