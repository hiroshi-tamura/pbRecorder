#pragma once
#include "core/Types.h"

namespace pb {

class ICaptureSource {
public:
    virtual ~ICaptureSource() = default;
    virtual bool initialize(const CaptureConfig& config, ID3D11Device* device) = 0;
    virtual bool start() = 0;
    virtual bool stop() = 0;
    virtual void setFrameCallback(FrameCallback callback) = 0;
    virtual void setErrorCallback(ErrorCallback callback) = 0;
    virtual uint32_t getWidth() const = 0;
    virtual uint32_t getHeight() const = 0;
    virtual bool isCapturing() const = 0;
};

} // namespace pb
