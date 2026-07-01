#pragma once

#include "capture/ICaptureSource.h"

// windows.h (and d3d11.h) must be included BEFORE the cppwinrt projection
// headers: they pull in <unknwn.h>, which is what enables cppwinrt's support
// for QueryInterface-ing classic COM interfaces (ID3D11Texture2D,
// IDirect3DDxgiInterfaceAccess, ...) via winrt::com_ptr::as<>() / .as<>().
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>
#include <string>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace pb {

// Window capture via Windows.Graphics.Capture (WGC).
//
// Replaces the PrintWindow-based WindowCapture for window mode. WGC captures
// the live, composited window content (including hardware-accelerated / DirectX
// child content that PrintWindow cannot render) directly into a GPU texture on
// the *same* D3D11 device the rest of pbRecorder uses, so no cross-device copy
// is needed. It also never includes overlapping windows.
//
// Apartment / threading strategy
// ------------------------------
// All WGC object creation, StartCapture and teardown happen on a private worker
// thread (wgcThread_) that initializes a *multi-threaded* apartment
// (winrt::init_apartment(apartment_type::multi_threaded)). This is deliberate:
//   * The Qt GUI thread has COM initialized as STA (Qt calls OleInitialize).
//     Calling init_apartment(multi_threaded) there would throw
//     RPC_E_CHANGED_MODE, and we must never disturb Qt's apartment.
//   * A fresh, owned worker thread starts un-initialized, so MTA init always
//     succeeds and is balanced by uninit_apartment() on the same thread.
//   * We use Direct3D11CaptureFramePool::CreateFreeThreaded, so FrameArrived is
//     raised on Windows thread-pool (implicitly MTA) threads and we do NOT need
//     a DispatcherQueue / message pump. The worker thread simply holds the MTA
//     (and the WGC object lifetimes) alive and blocks until stop() is requested.
class WgcWindowCapture : public ICaptureSource {
public:
    WgcWindowCapture();
    ~WgcWindowCapture() override;

    bool initialize(const CaptureConfig& config, ID3D11Device* device) override;
    bool start() override;
    bool stop() override;
    void setFrameCallback(FrameCallback callback) override;
    void setErrorCallback(ErrorCallback callback) override;
    uint32_t getWidth() const override;
    uint32_t getHeight() const override;
    bool isCapturing() const override;

    // True when Windows.Graphics.Capture is available on this OS (Win10 1903+).
    // Safe to call from any thread and regardless of the caller's COM state.
    static bool isSupported();

private:
    // --- worker-thread lifecycle -------------------------------------------
    void wgcThreadMain();
    void setupCapture();       // creates item/pool/session, StartCapture; throws on failure
    void teardownCapture();    // revokes handlers, closes session/pool; idempotent

    // --- WGC event handlers (run on thread-pool threads) -------------------
    void onFrameArrived(
        winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender,
        winrt::Windows::Foundation::IInspectable const& args);
    void onItemClosed(
        winrt::Windows::Graphics::Capture::GraphicsCaptureItem const& sender,
        winrt::Windows::Foundation::IInspectable const& args);

    // --- helpers ------------------------------------------------------------
    void deliverLatestFrame();   // re-delivers the stashed frame at CFR
    bool getWindowContentSize(uint32_t& width, uint32_t& height) const;
    int64_t queryTimestamp() const;
    void reportError(const std::string& msg);

    // --- config / D3D -------------------------------------------------------
    CaptureConfig config_;
    HWND hwnd_ = nullptr;
    ComPtr<ID3D11Device> device_;          // app device (shared with pipeline)
    ComPtr<ID3D11DeviceContext> context_;  // immediate context (multithread-protected)

    std::atomic<uint32_t> width_{0};       // even, visible window size
    std::atomic<uint32_t> height_{0};
    std::atomic<bool> capturing_{false};
    int64_t qpcFrequency_ = 0;
    int targetFps_ = 60;                    // CFR delivery rate

    // Latest captured frame. WGC delivers frames only when the window content
    // changes (event-driven), but a recorder needs a constant frame rate even
    // for static windows. onFrameArrived stashes the newest cropped texture
    // here; the worker thread re-delivers it to the pipeline at targetFps_ so
    // the output is CFR regardless of how often the source updates.
    std::mutex frameMutex_;
    ComPtr<ID3D11Texture2D> lastTexture_;
    uint32_t lastFrameW_ = 0;
    uint32_t lastFrameH_ = 0;
    bool hasFrame_ = false;

    // --- WGC objects (owned by the worker thread) --------------------------
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice rtDevice_{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item_{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool framePool_{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_{nullptr};
    winrt::event_token frameArrivedToken_{};
    winrt::event_token itemClosedToken_{};
    winrt::Windows::Graphics::SizeInt32 lastContentSize_{0, 0};

    // --- worker-thread control ---------------------------------------------
    std::thread wgcThread_;
    std::mutex stopMutex_;
    std::condition_variable stopCv_;
    bool stopRequested_ = false;
    std::promise<bool> setupPromise_;      // start() waits on this for setup result

    // Fences the pool-thread WGC handlers (onFrameArrived/onItemClosed) against
    // teardown. Revoking a WinRT event handler does NOT wait for a handler
    // already running on a thread-pool thread, so without this a handler could
    // still be dereferencing framePool_/rtDevice_/device_/this after
    // teardownCapture() nulls them or the object is destroyed (UAF). Held for
    // the whole handler body; teardownCapture() acquires it before revoking/
    // closing/nulling, so teardown blocks until any in-flight handler exits.
    std::mutex teardownMutex_;

    // --- callbacks ----------------------------------------------------------
    mutable std::mutex callbackMutex_;
    FrameCallback frameCallback_;
    ErrorCallback errorCallback_;
};

} // namespace pb
