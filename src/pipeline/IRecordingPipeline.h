#pragma once
#include "core/Types.h"

namespace pb {

class IRecordingPipeline {
public:
    virtual ~IRecordingPipeline() = default;
    virtual bool initialize(const RecordingConfig& config, ID3D11Device* device) = 0;
    virtual bool start() = 0;
    virtual bool stop() = 0;
    virtual bool writeVideoFrame(const VideoFrame& frame) = 0;
    virtual bool writeAudioSamples(const AudioBuffer& buffer) = 0;
    virtual void setErrorCallback(ErrorCallback callback) = 0;
    virtual bool isRecording() const = 0;
    virtual int64_t getDurationMs() const = 0;
    virtual int64_t getFileSize() const = 0;
};

} // namespace pb
