#pragma once
#include "core/Types.h"

namespace pb {

class IAudioSource {
public:
    virtual ~IAudioSource() = default;
    virtual bool initialize(const AudioDeviceInfo& device) = 0;
    virtual bool start() = 0;
    virtual bool stop() = 0;
    virtual void setAudioCallback(AudioCallback callback) = 0;
    virtual void setErrorCallback(ErrorCallback callback) = 0;
    virtual int getChannelCount() const = 0;
    virtual int getSampleRate() const = 0;
    virtual int getBitsPerSample() const = 0;
    virtual bool isCapturing() const = 0;
};

} // namespace pb
