#pragma once

#include "IAudioSource.h"

#include <thread>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

namespace pb {

class WasapiCapture : public IAudioSource {
public:
    WasapiCapture();
    ~WasapiCapture() override;

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

    // Device enumeration
    static std::vector<AudioDeviceInfo> enumerateDevices();

private:
    void captureThread();
    bool initializeDevice(const AudioDeviceInfo& device);
    void releaseResources();
    void reportError(const std::string& msg);

    // COM interfaces (raw pointers, manually released)
    IMMDevice* device_ = nullptr;
    IAudioClient* audioClient_ = nullptr;
    IAudioCaptureClient* captureClient_ = nullptr;

    HANDLE eventHandle_ = nullptr;
    HANDLE stopEvent_ = nullptr;

    WAVEFORMATEX* mixFormat_ = nullptr;

    std::thread captureThread_;
    std::atomic<bool> capturing_{false};
    std::atomic<bool> initialized_{false};

    mutable std::mutex mutex_;
    AudioCallback audioCallback_;
    ErrorCallback errorCallback_;

    // Device info
    AudioDeviceInfo deviceInfo_;
    int channelCount_ = 0;
    int sampleRate_ = 0;
    int bitsPerSample_ = 0;
    bool isLoopback_ = false;

    // Timestamp tracking
    int64_t startQpc_ = 0;
    int64_t qpcFrequency_ = 0;
};

} // namespace pb
