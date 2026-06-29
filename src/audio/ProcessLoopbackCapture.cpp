#include "ProcessLoopbackCapture.h"

#include <cstring>
#include <stdexcept>

// Process-loopback activation parameters. Use the SDK header when available,
// otherwise define the needed types manually (some MinGW SDKs lack the header).
#if defined(__has_include)
#  if __has_include(<audioclientactivationparams.h>)
#    include <audioclientactivationparams.h>
#    define PB_HAS_ACTIVATION_HEADER 1
#  endif
#endif

#ifndef PB_HAS_ACTIVATION_HEADER
typedef enum {
    AUDIOCLIENT_ACTIVATION_TYPE_DEFAULT = 0,
    AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK = 1
} AUDIOCLIENT_ACTIVATION_TYPE;

typedef enum {
    PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE = 0,
    PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE = 1
} PROCESS_LOOPBACK_MODE;

typedef struct {
    DWORD TargetProcessId;
    PROCESS_LOOPBACK_MODE ProcessLoopbackMode;
} AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS;

typedef struct {
    AUDIOCLIENT_ACTIVATION_TYPE ActivationType;
    union {
        AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS ProcessLoopbackParams;
    };
} AUDIOCLIENT_ACTIVATION_PARAMS;
#endif

#ifndef VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK
#define VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK L"VAD\\Process_Loopback"
#endif

namespace pb {

template <typename T>
static void safeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

struct ScopedCom {
    HRESULT hr;
    ScopedCom() { hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~ScopedCom() { if (SUCCEEDED(hr)) CoUninitialize(); }
    bool ok() const { return SUCCEEDED(hr); }
};

ProcessLoopbackCapture::ProcessLoopbackCapture() {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    qpcFrequency_ = freq.QuadPart;
}

ProcessLoopbackCapture::~ProcessLoopbackCapture() {
    stop();
    releaseResources();
}

// ============================================================================
// IUnknown — lifetime is owned by the enclosing unique_ptr, not by COM refcount.
// AddRef/Release only track the count; they never delete the object (the async
// activation always completes inside initialize() before the object is freed).
// ============================================================================
STDMETHODIMP ProcessLoopbackCapture::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) ||
        riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
        *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) ProcessLoopbackCapture::AddRef() {
    return refCount_.fetch_add(1) + 1;
}

STDMETHODIMP_(ULONG) ProcessLoopbackCapture::Release() {
    ULONG c = refCount_.fetch_sub(1) - 1;
    return c; // intentionally no delete
}

// ============================================================================
// ActivateCompleted — receives the async IAudioClient activation result.
// ============================================================================
STDMETHODIMP ProcessLoopbackCapture::ActivateCompleted(
        IActivateAudioInterfaceAsyncOperation* op) {
    HRESULT hrActivate = E_UNEXPECTED;
    IUnknown* punk = nullptr;
    HRESULT hr = op->GetActivateResult(&hrActivate, &punk);
    if (SUCCEEDED(hr) && SUCCEEDED(hrActivate) && punk) {
        punk->QueryInterface(__uuidof(IAudioClient),
                             reinterpret_cast<void**>(&audioClient_));
    }
    activateResult_ = SUCCEEDED(hr) ? hrActivate : hr;
    safeRelease(punk);
    if (activateDoneEvent_) SetEvent(activateDoneEvent_);
    return S_OK;
}

// ============================================================================
// activateInterface — kick off async activation and block until it completes.
// ============================================================================
bool ProcessLoopbackCapture::activateInterface(uint32_t processId,
                                               bool excludeProcessTree) {
    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = static_cast<DWORD>(processId);
    params.ProcessLoopbackParams.ProcessLoopbackMode =
        excludeProcessTree ? PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE
                           : PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT activateParams{};
    activateParams.vt = VT_BLOB;
    activateParams.blob.cbSize = sizeof(params);
    activateParams.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    activateResult_ = E_PENDING;
    IActivateAudioInterfaceAsyncOperation* asyncOp = nullptr;
    HRESULT hr = ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
        __uuidof(IAudioClient),
        &activateParams,
        this,
        &asyncOp);
    if (FAILED(hr)) {
        reportError("ActivateAudioInterfaceAsync failed: " + hrToString(hr));
        return false;
    }

    // Block until ActivateCompleted runs (it sets audioClient_/activateResult_).
    WaitForSingleObject(activateDoneEvent_, 5000);
    safeRelease(asyncOp);

    if (FAILED(activateResult_) || !audioClient_) {
        reportError("Process loopback activation failed: " + hrToString(activateResult_));
        return false;
    }
    return true;
}

// ============================================================================
// initialize
// ============================================================================
bool ProcessLoopbackCapture::initialize(const AudioDeviceInfo& device) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (capturing_) return false;
    releaseResources();

    if (device.processId == 0) {
        reportError("Process loopback: no target process id");
        return false;
    }

    eventHandle_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    activateDoneEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!eventHandle_ || !stopEvent_ || !activateDoneEvent_) {
        reportError("Process loopback: failed to create event handles");
        return false;
    }

    if (!activateInterface(device.processId, /*excludeProcessTree=*/false)) {
        return false;
    }

    // Fixed capture format: 16-bit PCM stereo 48 kHz.
    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = static_cast<WORD>(channelCount_);
    wfx.nSamplesPerSec = static_cast<DWORD>(sampleRate_);
    wfx.wBitsPerSample = static_cast<WORD>(bitsPerSample_);
    wfx.nBlockAlign = static_cast<WORD>(wfx.nChannels * wfx.wBitsPerSample / 8);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize = 0;

    HRESULT hr = audioClient_->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        200000,  // 20ms buffer
        0,
        &wfx,
        nullptr);
    if (FAILED(hr)) {
        reportError("Process loopback IAudioClient::Initialize failed: " + hrToString(hr));
        return false;
    }

    hr = audioClient_->SetEventHandle(eventHandle_);
    if (FAILED(hr)) {
        reportError("Process loopback SetEventHandle failed: " + hrToString(hr));
        return false;
    }

    hr = audioClient_->GetService(__uuidof(IAudioCaptureClient),
                                  reinterpret_cast<void**>(&captureClient_));
    if (FAILED(hr) || !captureClient_) {
        reportError("Process loopback GetService(IAudioCaptureClient) failed: " + hrToString(hr));
        return false;
    }

    initialized_ = true;
    return true;
}

// ============================================================================
// start
// ============================================================================
bool ProcessLoopbackCapture::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || capturing_) return false;

    ResetEvent(stopEvent_);
    HRESULT hr = audioClient_->Start();
    if (FAILED(hr)) {
        reportError("Process loopback IAudioClient::Start failed: " + hrToString(hr));
        return false;
    }

    capturing_ = true;
    captureThread_ = std::thread(&ProcessLoopbackCapture::captureThread, this);
    return true;
}

// ============================================================================
// stop
// ============================================================================
bool ProcessLoopbackCapture::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!capturing_) return true;
        capturing_ = false;
        if (stopEvent_) SetEvent(stopEvent_);
    }
    if (captureThread_.joinable()) captureThread_.join();

    std::lock_guard<std::mutex> lock(mutex_);
    if (audioClient_) {
        audioClient_->Stop();
        audioClient_->Reset();
    }
    return true;
}

// ============================================================================
// captureThread — process loopback delivers our requested 16-bit PCM directly.
// ============================================================================
void ProcessLoopbackCapture::captureThread() {
    try {
    ScopedCom com;
    HANDLE waitHandles[2] = { eventHandle_, stopEvent_ };

    while (capturing_) {
        DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, 2000);
        if (waitResult == WAIT_OBJECT_0 + 1) break; // stop
        if (waitResult == WAIT_TIMEOUT) {
            if (!capturing_) break;
            continue;
        }
        if (waitResult == WAIT_FAILED) {
            reportError("Process loopback WaitForMultipleObjects failed");
            break;
        }

        UINT32 packetLength = 0;
        HRESULT hr = captureClient_->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) {
            reportError("Process loopback GetNextPacketSize failed: " + hrToString(hr));
            break;
        }

        while (packetLength > 0 && capturing_) {
            BYTE* data = nullptr;
            UINT32 numFrames = 0;
            DWORD flags = 0;
            UINT64 devicePosition = 0;
            UINT64 qpcPosition = 0;
            hr = captureClient_->GetBuffer(&data, &numFrames, &flags,
                                           &devicePosition, &qpcPosition);
            if (FAILED(hr)) {
                reportError("Process loopback GetBuffer failed: " + hrToString(hr));
                capturing_ = false;
                break;
            }
            struct ReleaseGuard {
                IAudioCaptureClient* client = nullptr;
                UINT32 frames = 0;
                bool active = false;
                ~ReleaseGuard() { if (active && client) client->ReleaseBuffer(frames); }
            } guard{captureClient_, numFrames, true};

            if (numFrames > 0) {
                int64_t timestamp = 0;
                const bool tsError = (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0;
                if (qpcPosition > 0 && !tsError) {
                    timestamp = static_cast<int64_t>(qpcPosition);
                } else {
                    LARGE_INTEGER now;
                    QueryPerformanceCounter(&now);
                    timestamp = static_cast<int64_t>(
                        (static_cast<double>(now.QuadPart) / qpcFrequency_) * 10000000.0);
                    timestamp -= static_cast<int64_t>(numFrames) * 10000000LL /
                                 static_cast<int64_t>(sampleRate_ > 0 ? sampleRate_ : 48000);
                }
                if (timestamp < 0) timestamp = 0;

                AudioBuffer buffer;
                buffer.sampleCount = numFrames;
                buffer.channelCount = channelCount_;
                buffer.sampleRate = sampleRate_;
                buffer.bitsPerSample = bitsPerSample_;
                buffer.timestamp = timestamp;

                uint32_t outSize = numFrames * channelCount_ * (bitsPerSample_ / 8);
                buffer.data.resize(outSize);
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    std::memset(buffer.data.data(), 0, outSize);
                } else {
                    std::memcpy(buffer.data.data(), data, outSize);
                }

                AudioCallback cb;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    cb = audioCallback_;
                }
                if (cb) {
                    try {
                        cb(buffer);
                    } catch (const std::exception& e) {
                        reportError(std::string("Process loopback callback failed: ") + e.what());
                        capturing_ = false;
                    } catch (...) {
                        reportError("Process loopback callback failed with an unknown error");
                        capturing_ = false;
                    }
                }
            }

            guard.active = false;
            captureClient_->ReleaseBuffer(numFrames);

            hr = captureClient_->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) { capturing_ = false; break; }
        }
    }
    } catch (const std::exception& e) {
        reportError(std::string("Process loopback capture thread failed: ") + e.what());
        capturing_ = false;
    } catch (...) {
        reportError("Process loopback capture thread failed with an unknown error");
        capturing_ = false;
    }
}

// ============================================================================
// Setters / Getters
// ============================================================================
void ProcessLoopbackCapture::setAudioCallback(AudioCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    audioCallback_ = std::move(callback);
}
void ProcessLoopbackCapture::setErrorCallback(ErrorCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    errorCallback_ = std::move(callback);
}
int ProcessLoopbackCapture::getChannelCount() const { return channelCount_; }
int ProcessLoopbackCapture::getSampleRate() const { return sampleRate_; }
int ProcessLoopbackCapture::getBitsPerSample() const { return bitsPerSample_; }
bool ProcessLoopbackCapture::isCapturing() const { return capturing_; }

// ============================================================================
// releaseResources
// ============================================================================
void ProcessLoopbackCapture::releaseResources() {
    safeRelease(captureClient_);
    safeRelease(audioClient_);
    if (eventHandle_) { CloseHandle(eventHandle_); eventHandle_ = nullptr; }
    if (stopEvent_) { CloseHandle(stopEvent_); stopEvent_ = nullptr; }
    if (activateDoneEvent_) { CloseHandle(activateDoneEvent_); activateDoneEvent_ = nullptr; }
    initialized_ = false;
}

void ProcessLoopbackCapture::reportError(const std::string& msg) {
    ErrorCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = errorCallback_;
    }
    if (cb) cb(msg);
}

} // namespace pb
