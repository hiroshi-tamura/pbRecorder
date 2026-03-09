#pragma once

#include "IAudioSource.h"

#include <thread>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <functional>

#ifdef ASIO_AVAILABLE
#include <objbase.h>   // Defines 'interface' keyword needed by ASIO SDK on MinGW
#include "asiosys.h"   // Must be included before asio.h to set IEEE754_64FLOAT
#include "asio.h"
#include "iasiodrv.h"
#include "asiodrivers.h"
#endif

#include <windows.h>

namespace pb {

class AsioCapture : public IAudioSource {
public:
    AsioCapture();
    ~AsioCapture() override;

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

    // Device enumeration (scans registry for ASIO drivers)
    static std::vector<AudioDeviceInfo> enumerateDevices();

private:
#ifdef ASIO_AVAILABLE
    // ASIO callback trampolines (static, forward to singleton instance)
    static void bufferSwitchCallback(long index, ASIOBool processNow);
    static void sampleRateDidChangeCallback(ASIOSampleRate sRate);
    static long asioMessageCallback(long selector, long value, void* message, double* opt);
    static ASIOTime* bufferSwitchTimeInfoCallback(ASIOTime* timeInfo, long index, ASIOBool processNow);

    void onBufferSwitch(long index, ASIOBool processNow);
    void convertAndDeliver(long bufferIndex);

    // ASIO driver state
    AsioDrivers* asioDrivers_ = nullptr;
    ASIODriverInfo driverInfo_{};
    ASIOBufferInfo* bufferInfos_ = nullptr;
    ASIOChannelInfo* channelInfos_ = nullptr;
    long inputChannels_ = 0;
    long outputChannels_ = 0;
    long preferredBufferSize_ = 0;
    long minBufferSize_ = 0;
    long maxBufferSize_ = 0;
    long bufferGranularity_ = 0;
    ASIOSampleType sampleType_ = ASIOSTInt16LSB;
    ASIOCallbacks asioCallbacks_{};
    bool isOutputCapture_ = false;  // true = capture output channels
    long activeChannels_ = 0;       // total channels being captured

    static AsioCapture* instance_;
#endif

    void reportError(const std::string& msg);
    void releaseResources();

    // Dedicated STA thread for ASIO COM operations
    // ASIO4ALL requires STA, but main thread uses MTA for WASAPI
    void startAsioThread();
    void stopAsioThread();
    void asioThreadFunc();
    void runOnAsioThread(std::function<void()> task);
    bool initializeOnAsioThread(const AudioDeviceInfo& device);

    std::thread asioThread_;
    HANDLE asioTaskEvent_ = nullptr;
    HANDLE asioTaskDoneEvent_ = nullptr;
    std::function<void()> pendingTask_;
    std::atomic<bool> asioThreadRunning_{false};

    std::atomic<bool> capturing_{false};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> inCallback_{false};

    mutable std::mutex mutex_;
    AudioCallback audioCallback_;
    ErrorCallback errorCallback_;

    AudioDeviceInfo deviceInfo_;
    int channelCount_ = 0;
    int sampleRate_ = 0;
    int bitsPerSample_ = 16;
};

} // namespace pb
