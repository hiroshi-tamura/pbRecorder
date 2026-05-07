#pragma once

#include "core/Types.h"

#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace pb {

struct EncodedVideoPacket {
    std::vector<uint8_t> data;
    int64_t timestampMs = 0;
    int64_t durationMs = 0;
    bool keyframe = false;
};

class H264MftEncoder {
public:
    H264MftEncoder() = default;
    ~H264MftEncoder();

    bool initialize(const RecordingConfig& config, ID3D11Device* device);
    bool encodeFrame(const VideoFrame& frame, std::vector<EncodedVideoPacket>& packets);
    bool drain(std::vector<EncodedVideoPacket>& packets);
    void shutdown();
    const std::string& lastError() const { return lastError_; }

private:
    bool createEncoder();
    bool configureOutputType();
    bool configureInputType();
    bool ensureStagingTexture(uint32_t width, uint32_t height);
    bool copyFrameToNv12(const VideoFrame& frame, std::vector<uint8_t>& nv12);
    bool processOutput(std::vector<EncodedVideoPacket>& packets);

    RecordingConfig config_;
    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dContext_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> stagingTexture_;
    Microsoft::WRL::ComPtr<IMFTransform> encoder_;

    std::string lastError_;
    uint32_t stagingWidth_ = 0;
    uint32_t stagingHeight_ = 0;
    int64_t nextOutputTimestampMs_ = -1;
    bool mfStarted_ = false;
    bool comInitialized_ = false;
    bool initialized_ = false;
};

} // namespace pb
