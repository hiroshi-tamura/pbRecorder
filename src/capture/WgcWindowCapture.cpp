#include "capture/WgcWindowCapture.h"

// Classic-COM interop for WGC. These headers declare:
//   * IGraphicsCaptureItemInterop::CreateForWindow (HWND -> GraphicsCaptureItem)
//   * CreateDirect3D11DeviceFromDXGIDevice (ID3D11Device -> WinRT IDirect3DDevice)
//   * IDirect3DDxgiInterfaceAccess::GetInterface (IDirect3DSurface -> ID3D11Texture2D)
// They must be included AFTER the cppwinrt projection headers (pulled in by the
// class header) so the projected and ABI types coexist correctly.
#include <inspectable.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/Windows.Graphics.DirectX.h>   // DirectXPixelFormat

#include <dxgi.h>
#include <dwmapi.h>
#include <roapi.h>

#include <algorithm>
#include <chrono>
#include <exception>

namespace pb {

using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
using winrt::Windows::Foundation::IInspectable;
using winrt::Windows::Foundation::TimeSpan;
using winrt::Windows::Graphics::SizeInt32;

// ============================================================================
// Construction / destruction
// ============================================================================

WgcWindowCapture::WgcWindowCapture() {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    qpcFrequency_ = freq.QuadPart;
}

WgcWindowCapture::~WgcWindowCapture() {
    stop();
}

// ============================================================================
// isSupported()
// ============================================================================

bool WgcWindowCapture::isSupported() {
    // GraphicsCaptureSession::IsSupported() reaches into the activation factory,
    // which requires COM to be initialized on the calling thread. The GUI thread
    // already is (STA), but the CLI thread may not be, so guard with a scoped
    // RoInitialize. We use RoInitialize (not winrt::init_apartment) because it
    // does not throw on RPC_E_CHANGED_MODE (thread already STA), which is fine —
    // COM is usable either way.
    bool needUninit = false;
    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        // S_OK (we initialized it) or S_FALSE (already initialized, same mode);
        // both increment the ref count and must be balanced.
        needUninit = true;
    }
    // hr == RPC_E_CHANGED_MODE => already initialized in a different mode; do not
    // uninit, but COM is still usable for the probe below.

    bool supported = false;
    try {
        supported = GraphicsCaptureSession::IsSupported();
    } catch (...) {
        supported = false;
    }

    if (needUninit) {
        RoUninitialize();
    }
    return supported;
}

// ============================================================================
// initialize()
// ============================================================================

bool WgcWindowCapture::initialize(const CaptureConfig& config, ID3D11Device* device) {
    if (!device) {
        reportError("WgcWindowCapture::initialize: device is null");
        return false;
    }
    if (!config.targetWindow || !IsWindow(config.targetWindow)) {
        reportError("WgcWindowCapture::initialize: invalid target window handle");
        return false;
    }

    config_ = config;
    hwnd_ = config.targetWindow;
    device_ = device;
    device_->GetImmediateContext(&context_);
    targetFps_ = std::clamp(config.targetFps, 1, 240);

    // Report the visible-window size (DWM extended frame bounds), even, exactly
    // like WindowCapture, so RecordingSession picks identical encoder dimensions.
    // WGC content already excludes the invisible resize-border padding that
    // GetWindowRect/PrintWindow include, so no border offset is needed here.
    uint32_t w = 0, h = 0;
    if (!getWindowContentSize(w, h) || w == 0 || h == 0) {
        reportError("WgcWindowCapture::initialize: could not get window size");
        return false;
    }
    width_.store(w);
    height_.store(h);

    // Intentionally do NOT touch WinRT here — all WGC setup happens on the
    // worker thread in start() to keep the (STA) GUI thread's apartment intact
    // and to keep WGC object lifetimes on a single MTA thread.
    return true;
}

// ============================================================================
// start() / stop()
// ============================================================================

bool WgcWindowCapture::start() {
    if (wgcThread_.joinable()) {
        return true; // already started
    }

    {
        std::lock_guard<std::mutex> lk(stopMutex_);
        stopRequested_ = false;
    }

    setupPromise_ = std::promise<bool>();
    std::future<bool> setupResult = setupPromise_.get_future();

    wgcThread_ = std::thread([this]() { wgcThreadMain(); });

    // Block until the worker reports whether WGC setup succeeded, so we can
    // return an accurate bool (RecordingSession aborts recording on false).
    bool ok = false;
    try {
        ok = setupResult.get();
    } catch (...) {
        ok = false;
    }

    if (!ok) {
        {
            std::lock_guard<std::mutex> lk(stopMutex_);
            stopRequested_ = true;
        }
        stopCv_.notify_all();
        if (wgcThread_.joinable()) {
            wgcThread_.join();
        }
        return false;
    }
    return true;
}

bool WgcWindowCapture::stop() {
    {
        std::lock_guard<std::mutex> lk(stopMutex_);
        stopRequested_ = true;
    }
    stopCv_.notify_all();

    if (wgcThread_.joinable()) {
        wgcThread_.join();  // the worker performs teardown before returning
    }
    capturing_.store(false);
    return true;
}

// ============================================================================
// Worker thread
// ============================================================================

void WgcWindowCapture::wgcThreadMain() {
    bool apartmentInited = false;
    bool setupOk = false;

    try {
        // Dedicated MTA on this owned thread (see header for rationale).
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        apartmentInited = true;

        setupCapture();   // throws on any failure
        setupOk = true;
    } catch (const winrt::hresult_error& e) {
        reportError("WgcWindowCapture setup failed: " +
                    winrt::to_string(e.message()));
    } catch (const std::exception& e) {
        reportError(std::string("WgcWindowCapture setup failed: ") + e.what());
    } catch (...) {
        reportError("WgcWindowCapture setup failed with an unknown error");
    }

    // Hand the result back to start() exactly once.
    try {
        setupPromise_.set_value(setupOk);
    } catch (...) {
        // set_value can only throw if already satisfied; ignore.
    }

    if (setupOk) {
        // CFR delivery loop. FrameArrived (on pool threads) stashes the latest
        // cropped texture; here we re-deliver it to the pipeline at targetFps_,
        // so a static window still yields a constant frame rate. Sampling the
        // latest frame at a fixed cadence also keeps video on the same steady
        // clock as audio (tight A/V sync).
        const auto interval = std::chrono::microseconds(1000000 / targetFps_);
        std::unique_lock<std::mutex> lk(stopMutex_);
        while (!stopRequested_) {
            // Wait one frame interval, waking early if stop is requested.
            stopCv_.wait_for(lk, interval, [this] { return stopRequested_; });
            if (stopRequested_) break;
            lk.unlock();
            deliverLatestFrame();
            lk.lock();
        }
    }

    teardownCapture();

    if (apartmentInited) {
        winrt::uninit_apartment();
    }
}

void WgcWindowCapture::setupCapture() {
    // 1) Wrap the EXISTING app D3D11 device as a WinRT IDirect3DDevice, so the
    //    frame pool allocates its surfaces on the very device the encoder and
    //    the rest of the app use. This lets us copy captured frames with a plain
    //    same-device CopySubresourceRegion (no shared-handle / cross-device copy).
    winrt::com_ptr<IDXGIDevice> dxgiDevice;
    winrt::check_hresult(device_->QueryInterface(
        __uuidof(IDXGIDevice), dxgiDevice.put_void()));

    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(
        CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put()));
    rtDevice_ = inspectable.as<IDirect3DDevice>();

    // 2) Create the capture item from the HWND via the interop factory.
    auto factory = winrt::get_activation_factory<GraphicsCaptureItem>();
    auto interop = factory.as<IGraphicsCaptureItemInterop>();
    winrt::check_hresult(interop->CreateForWindow(
        hwnd_,
        winrt::guid_of<GraphicsCaptureItem>(),
        winrt::put_abi(item_)));

    // 3) Free-threaded frame pool: FrameArrived on thread-pool threads, no
    //    DispatcherQueue required. BGRA8 UNorm == DXGI_FORMAT_B8G8R8A8_UNORM,
    //    matching the pipeline's MFVideoFormat_RGB32 input and WindowCapture's
    //    DXGI_FORMAT_B8G8R8A8_UNORM output.
    SizeInt32 size = item_.Size();
    if (size.Width < 1)  size.Width = 1;
    if (size.Height < 1) size.Height = 1;
    lastContentSize_ = size;

    framePool_ = Direct3D11CaptureFramePool::CreateFreeThreaded(
        rtDevice_,
        DirectXPixelFormat::B8G8R8A8UIntNormalized,
        2,          // two buffers is the recommended minimum
        size);

    frameArrivedToken_ = framePool_.FrameArrived({this, &WgcWindowCapture::onFrameArrived});

    session_ = framePool_.CreateCaptureSession(item_);
    itemClosedToken_ = item_.Closed({this, &WgcWindowCapture::onItemClosed});

    // Cursor toggle (IGraphicsCaptureSession2, Win10 2004+). Guard for older OS.
    try {
        session_.IsCursorCaptureEnabled(config_.captureCursor);
    } catch (...) {
        // Property not available on this build — ignore.
    }

    // Disable the yellow capture border (IGraphicsCaptureSession3, Win11).
    // Guard: the projected method QIs for the newer interface and throws on
    // older OSes where it does not exist.
    try {
        session_.IsBorderRequired(false);
    } catch (...) {
        // Border cannot be disabled on this build — ignore.
    }

    session_.StartCapture();

    capturing_.store(true);
}

void WgcWindowCapture::teardownCapture() {
    capturing_.store(false);

    // Acquire the fence before touching any WGC object: this blocks until any
    // in-flight onFrameArrived/onItemClosed on a thread-pool thread has exited,
    // so nothing dereferences framePool_/rtDevice_/etc. after we close/null them
    // (or after the object is destroyed). Runs on the worker thread, which is
    // off the thread-pool, so it only waits — never self-deadlocks.
    std::lock_guard<std::mutex> life(teardownMutex_);

    try {
        if (framePool_ && frameArrivedToken_.value) {
            framePool_.FrameArrived(frameArrivedToken_);   // revoke
            frameArrivedToken_ = {};
        }
    } catch (...) {}
    try {
        if (item_ && itemClosedToken_.value) {
            item_.Closed(itemClosedToken_);                // revoke
            itemClosedToken_ = {};
        }
    } catch (...) {}
    try { if (session_)   session_.Close();   } catch (...) {}
    try { if (framePool_) framePool_.Close(); } catch (...) {}

    session_   = nullptr;
    framePool_ = nullptr;
    item_      = nullptr;
    rtDevice_  = nullptr;
}

// ============================================================================
// FrameArrived — runs on a thread-pool (MTA) thread
// ============================================================================

void WgcWindowCapture::onFrameArrived(
    Direct3D11CaptureFramePool const& sender,
    IInspectable const& /*args*/) {

    // Hold the teardown fence for the whole body so teardownCapture() cannot
    // null/close the WGC objects (or destroy this) while we use them. Re-check
    // capturing_ under the lock: if teardown already ran, we bail before
    // touching any torn-down member.
    std::lock_guard<std::mutex> life(teardownMutex_);
    if (!capturing_.load()) {
        return;
    }

    try {
        Direct3D11CaptureFrame frame = sender.TryGetNextFrame();
        if (!frame) {
            return;
        }

        // Get the underlying ID3D11Texture2D of the captured surface. It lives on
        // rtDevice_, i.e. the app's D3D11 device, so it is directly copyable with
        // our immediate context.
        auto access = frame.Surface().as<
            ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        ComPtr<ID3D11Texture2D> frameTexture;
        winrt::check_hresult(access->GetInterface(
            __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(frameTexture.GetAddressOf())));

        // Determine the valid content region. WGC gives us a surface sized to the
        // frame pool; ContentSize() is the true window size. When the window grew
        // past the pool, ContentSize exceeds the surface — clamp to both. Round
        // down to even for H.264.
        const SizeInt32 contentSize = frame.ContentSize();

        D3D11_TEXTURE2D_DESC srcDesc = {};
        frameTexture->GetDesc(&srcDesc);

        uint32_t cropW = std::min<uint32_t>(
            static_cast<uint32_t>(contentSize.Width < 0 ? 0 : contentSize.Width),
            srcDesc.Width);
        uint32_t cropH = std::min<uint32_t>(
            static_cast<uint32_t>(contentSize.Height < 0 ? 0 : contentSize.Height),
            srcDesc.Height);
        cropW &= ~1u;
        cropH &= ~1u;

        if (cropW == 0 || cropH == 0) {
            // Nothing valid this frame; still handle resize below.
        } else {
            // App-owned destination texture, matching WindowCapture's output:
            // DXGI_FORMAT_B8G8R8A8_UNORM, DEFAULT usage, SHADER_RESOURCE bind.
            // A fresh texture per frame (like WindowCapture/DxgiScreenCapture) —
            // the frame is queued and consumed asynchronously, so we must not
            // reuse a single texture.
            D3D11_TEXTURE2D_DESC dstDesc = {};
            dstDesc.Width = cropW;
            dstDesc.Height = cropH;
            dstDesc.MipLevels = 1;
            dstDesc.ArraySize = 1;
            dstDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            dstDesc.SampleDesc.Count = 1;
            dstDesc.Usage = D3D11_USAGE_DEFAULT;
            dstDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            ComPtr<ID3D11Texture2D> dst;
            HRESULT hr = device_->CreateTexture2D(&dstDesc, nullptr, dst.GetAddressOf());
            if (SUCCEEDED(hr)) {
                D3D11_BOX srcBox = {};
                srcBox.left = 0;
                srcBox.top = 0;
                srcBox.front = 0;
                srcBox.right = cropW;
                srcBox.bottom = cropH;
                srcBox.back = 1;

                // Same-device GPU copy of the visible region (top-left origin).
                // The immediate context is multithread-protected (D3DManager),
                // so concurrent use with the pipeline writer thread is safe.
                context_->CopySubresourceRegion(
                    dst.Get(), 0, 0, 0, 0,
                    frameTexture.Get(), 0, &srcBox);

                width_.store(cropW);
                height_.store(cropH);

                // Stash as the latest frame. The worker thread re-delivers it to
                // the pipeline at targetFps_ (CFR), so we do NOT call the frame
                // callback here — WGC only fires on content change, which would
                // otherwise starve static windows of frames.
                std::lock_guard<std::mutex> lock(frameMutex_);
                lastTexture_ = dst;
                lastFrameW_ = cropW;
                lastFrameH_ = cropH;
                hasFrame_ = true;
            }
        }

        // Handle resize: if the window's content size changed, recreate the frame
        // pool for the new size so subsequent frames are captured at full size.
        // Recreate() is documented as safe to call from within FrameArrived. We
        // do it AFTER copying so the current (already-acquired) frame stays valid.
        if (contentSize.Width != lastContentSize_.Width ||
            contentSize.Height != lastContentSize_.Height) {
            SizeInt32 newSize = contentSize;
            if (newSize.Width < 1)  newSize.Width = 1;
            if (newSize.Height < 1) newSize.Height = 1;
            lastContentSize_ = contentSize;
            // Use the strong-ref `sender` parameter (not the framePool_ member)
            // so the call never targets a member being nulled by teardown.
            sender.Recreate(
                rtDevice_,
                DirectXPixelFormat::B8G8R8A8UIntNormalized,
                2,
                newSize);
        }

        // `frame` releases its pool buffer when it goes out of scope here.
    } catch (const winrt::hresult_error& e) {
        // Typically raised if we race a teardown (pool/session closed); benign.
        if (capturing_.load()) {
            reportError("WgcWindowCapture FrameArrived failed: " +
                        winrt::to_string(e.message()));
        }
    } catch (...) {
        // Swallow — never let an exception escape into the WGC dispatcher.
    }
}

void WgcWindowCapture::onItemClosed(
    GraphicsCaptureItem const& /*sender*/,
    IInspectable const& /*args*/) {
    // The target window was closed/destroyed. Report and ask the worker thread to
    // tear down. We do NOT join here (that is the owner's stop() job); we only
    // signal, so teardown runs exactly once on the worker.
    //
    // Hold the teardown fence so this pool-thread handler cannot run while the
    // object is being destroyed. Lock order teardownMutex_ -> stopMutex_ is the
    // only nesting; teardownCapture() takes only teardownMutex_, so no cycle.
    std::lock_guard<std::mutex> life(teardownMutex_);
    if (!capturing_.load()) {
        return;  // already tearing down
    }
    reportError("WgcWindowCapture: target window was closed");
    capturing_.store(false);
    {
        std::lock_guard<std::mutex> lk(stopMutex_);
        stopRequested_ = true;
    }
    stopCv_.notify_all();
}

// ============================================================================
// Accessors / helpers
// ============================================================================

void WgcWindowCapture::setFrameCallback(FrameCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    frameCallback_ = std::move(callback);
}

void WgcWindowCapture::setErrorCallback(ErrorCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    errorCallback_ = std::move(callback);
}

uint32_t WgcWindowCapture::getWidth() const { return width_.load(); }
uint32_t WgcWindowCapture::getHeight() const { return height_.load(); }
bool WgcWindowCapture::isCapturing() const { return capturing_.load(); }

void WgcWindowCapture::deliverLatestFrame() {
    if (!capturing_.load()) {
        return;
    }

    // Grab the latest stashed texture. Reusing the same texture across ticks
    // (static window) is safe: the pipeline only reads it (GPU copy into the
    // encoder input); the ComPtr keeps it alive.
    ComPtr<ID3D11Texture2D> tex;
    uint32_t w = 0, h = 0;
    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        if (!hasFrame_ || !lastTexture_) {
            return;  // no frame captured yet
        }
        tex = lastTexture_;
        w = lastFrameW_;
        h = lastFrameH_;
    }

    VideoFrame vf;
    vf.texture = tex;
    vf.timestamp = queryTimestamp();  // fresh QPC 100ns => steady CFR spacing
    vf.width = w;
    vf.height = h;

    FrameCallback cb;
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        cb = frameCallback_;
    }
    if (cb) {
        try {
            cb(vf);
        } catch (const std::exception& e) {
            reportError(std::string("WgcWindowCapture callback failed: ") + e.what());
            capturing_.store(false);
        } catch (...) {
            reportError("WgcWindowCapture callback failed with an unknown error");
            capturing_.store(false);
        }
    }
}

bool WgcWindowCapture::getWindowContentSize(uint32_t& width, uint32_t& height) const {
    RECT r = {};
    HRESULT hr = DwmGetWindowAttribute(hwnd_, DWMWA_EXTENDED_FRAME_BOUNDS,
                                       &r, sizeof(r));
    if (FAILED(hr)) {
        if (!GetWindowRect(hwnd_, &r)) {
            return false;
        }
    }
    int cw = static_cast<int>(r.right - r.left);
    int ch = static_cast<int>(r.bottom - r.top);
    if (cw <= 0 || ch <= 0) {
        return false;
    }
    width = static_cast<uint32_t>(cw & ~1);   // even for H.264
    height = static_cast<uint32_t>(ch & ~1);
    return true;
}

int64_t WgcWindowCapture::queryTimestamp() const {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return static_cast<int64_t>(
        (static_cast<double>(counter.QuadPart) /
         static_cast<double>(qpcFrequency_)) * 10000000.0);
}

void WgcWindowCapture::reportError(const std::string& msg) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (errorCallback_) {
        errorCallback_(msg);
    }
}

} // namespace pb
