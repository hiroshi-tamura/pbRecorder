#pragma once

#include "core/Types.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

namespace pb {

struct EncodedAudioPacket {
    std::vector<uint8_t> data;
    int64_t timestampMs = 0;
    int64_t durationMs = 0;
};

class AacMftEncoder {
public:
    AacMftEncoder() = default;
    ~AacMftEncoder();

    bool initialize(const RecordingConfig& config);
    bool encode(const AudioBuffer& buffer, std::vector<EncodedAudioPacket>& packets);
    bool drain(std::vector<EncodedAudioPacket>& packets);
    void shutdown();

private:
    bool configureOutputType();
    bool configureInputType();
    bool processOutput(std::vector<EncodedAudioPacket>& packets);
    std::vector<int16_t> convertToPcm16(const AudioBuffer& buffer) const;

    RecordingConfig config_;
    Microsoft::WRL::ComPtr<IMFTransform> encoder_;
    int64_t firstTimestamp_ = -1;
    bool initialized_ = false;
};

} // namespace pb
