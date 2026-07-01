#pragma once

#include "IAudioSource.h"

#include <thread>
#include <atomic>
#include <mutex>
#include <string>

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

namespace pb {

// Captures the audio rendered by a single application (and its child processes)
// using the Windows process-loopback API (ActivateAudioInterfaceAsync with
// AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK). Requires Windows 10 build
// 20348+ (2004/20H1 on client builds). Output is fixed 16-bit PCM stereo 48kHz.
//
// The class also implements IActivateAudioInterfaceCompletionHandler so it can
// receive the async activation result.
class ProcessLoopbackCapture
    : public IAudioSource,
      public IActivateAudioInterfaceCompletionHandler {
public:
    ProcessLoopbackCapture();
    ~ProcessLoopbackCapture() override;

    // IAudioSource
    bool initialize(const AudioDeviceInfo& device) override;
    bool start() override;
    bool stop() override;
    void setAudioCallback(AudioCallback callback) override;
    void setErrorCallback(ErrorCallback callback) override;
    int getChannelCount() const override;
    int getSampleRate() const override;
    int getBitsPerSample() const override;
    bool isCapturing() const override;

    // IActivateAudioInterfaceCompletionHandler
    STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation* op) override;

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;

private:
    void captureThread();
    bool activateInterface(uint32_t processId, bool excludeProcessTree);
    void releaseResources();
    void reportError(const std::string& msg);

    // Free-threaded marshaler. ActivateAudioInterfaceAsync requires the
    // completion handler to support free-threaded marshaling; without it the
    // call fails synchronously with E_ILLEGAL_METHOD_CALL (0x8000000E) on an MTA
    // thread — which is why per-app audio never captured anything. We aggregate
    // the standard FTM and delegate IMarshal to it in QueryInterface.
    IUnknown* ftm_ = nullptr;

    IAudioClient* audioClient_ = nullptr;
    IAudioCaptureClient* captureClient_ = nullptr;

    HANDLE eventHandle_ = nullptr;
    HANDLE stopEvent_ = nullptr;
    HANDLE activateDoneEvent_ = nullptr;
    HRESULT activateResult_ = E_PENDING;

    std::thread captureThread_;
    std::atomic<bool> capturing_{false};
    std::atomic<bool> initialized_{false};
    std::atomic<ULONG> refCount_{1};

    mutable std::mutex mutex_;          // guards capture state (initialize/start/stop)
    // Separate mutex for the callbacks so reportError() can run while mutex_ is
    // held by initialize()/start(). std::mutex is non-recursive: re-locking it on
    // the same thread throws std::system_error (a crash), which is exactly what
    // happened when activation failed inside initialize() and reportError() tried
    // to re-lock mutex_.
    mutable std::mutex callbackMutex_;
    AudioCallback audioCallback_;
    ErrorCallback errorCallback_;

    // Fixed output format
    int channelCount_ = 2;
    int sampleRate_ = 48000;
    int bitsPerSample_ = 16;

    int64_t qpcFrequency_ = 0;
};

} // namespace pb
