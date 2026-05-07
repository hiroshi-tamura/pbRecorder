#include "pipeline/AacMftEncoder.h"

#include <mferror.h>

#include <algorithm>
#include <cstring>

namespace pb {

namespace {

constexpr GUID PB_CLSID_AACMFTEncoder =
    {0x93af0c51, 0x2275, 0x45d2, {0xa3, 0x5b, 0xf2, 0xba, 0x21, 0xca, 0xed, 0x00}};

int16_t floatToI16(float value)
{
    value = std::clamp(value, -1.0f, 1.0f);
    return static_cast<int16_t>(value * 32767.0f);
}

} // namespace

AacMftEncoder::~AacMftEncoder()
{
    shutdown();
}

bool AacMftEncoder::initialize(const RecordingConfig& config)
{
    config_ = config;
    lastError_.clear();

    HRESULT hr = CoCreateInstance(PB_CLSID_AACMFTEncoder,
                                  nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&encoder_));
    if (FAILED(hr) || !encoder_) {
        lastError_ = "CoCreateInstance AAC MFT failed: " + hrToString(hr);
        return false;
    }

    if (!configureOutputType() || !configureInputType()) {
        return false;
    }

    encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    firstTimestamp_ = -1;
    nextInputTimestamp_ = -1;
    initialized_ = true;
    return true;
}

bool AacMftEncoder::configureOutputType()
{
    Microsoft::WRL::ComPtr<IMFMediaType> type;
    PB_CHECK_HR(MFCreateMediaType(&type), "Failed to create AAC output type");
    PB_CHECK_HR(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio), "Failed to set audio major type");
    PB_CHECK_HR(type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC), "Failed to set AAC subtype");
    PB_CHECK_HR(type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, config_.audio.sampleRate),
                "Failed to set AAC sample rate");
    PB_CHECK_HR(type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, config_.audio.channelCount),
                "Failed to set AAC channels");
    PB_CHECK_HR(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16),
                "Failed to set AAC bits per sample");
    PB_CHECK_HR(type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, config_.audio.bitrate / 8),
                "Failed to set AAC bitrate");
    PB_CHECK_HR(type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0),
                "Failed to set AAC payload type");
    PB_CHECK_HR(type->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29),
                "Failed to set AAC profile");

    HRESULT hr = encoder_->SetOutputType(0, type.Get(), 0);
    if (FAILED(hr)) {
        lastError_ = "SetOutputType AAC failed: " + hrToString(hr);
        return false;
    }
    return true;
}

bool AacMftEncoder::configureInputType()
{
    const UINT32 channels = static_cast<UINT32>(config_.audio.channelCount);
    const UINT32 sampleRate = static_cast<UINT32>(config_.audio.sampleRate);
    const UINT32 blockAlign = channels * sizeof(int16_t);

    Microsoft::WRL::ComPtr<IMFMediaType> type;
    PB_CHECK_HR(MFCreateMediaType(&type), "Failed to create AAC input type");
    PB_CHECK_HR(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio), "Failed to set audio major type");
    PB_CHECK_HR(type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM), "Failed to set PCM subtype");
    PB_CHECK_HR(type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate),
                "Failed to set PCM sample rate");
    PB_CHECK_HR(type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels),
                "Failed to set PCM channels");
    PB_CHECK_HR(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16),
                "Failed to set PCM bits");
    PB_CHECK_HR(type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, blockAlign),
                "Failed to set PCM block alignment");
    PB_CHECK_HR(type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, sampleRate * blockAlign),
                "Failed to set PCM byte rate");

    HRESULT hr = encoder_->SetInputType(0, type.Get(), 0);
    if (FAILED(hr)) {
        lastError_ = "SetInputType AAC PCM failed: " + hrToString(hr);
        return false;
    }
    return true;
}

bool AacMftEncoder::encode(const AudioBuffer& buffer, std::vector<EncodedAudioPacket>& packets)
{
    if (!initialized_ || !encoder_ || buffer.data.empty()) {
        return true;
    }

    std::vector<int16_t> pcm = convertToPcm16(buffer);
    if (pcm.empty()) {
        return true;
    }

    const int64_t duration = static_cast<int64_t>(buffer.sampleCount) * 10000000LL /
                             static_cast<int64_t>(config_.audio.sampleRate);

    if (nextInputTimestamp_ < 0) {
        nextInputTimestamp_ = std::max<int64_t>(0, buffer.timestamp);
    }
    int64_t relativeTs = nextInputTimestamp_;
    nextInputTimestamp_ += duration;

    const DWORD dataSize = static_cast<DWORD>(pcm.size() * sizeof(int16_t));
    Microsoft::WRL::ComPtr<IMFMediaBuffer> mediaBuffer;
    PB_CHECK_HR(MFCreateMemoryBuffer(dataSize, &mediaBuffer), "Failed to create AAC input buffer");

    BYTE* dst = nullptr;
    PB_CHECK_HR(mediaBuffer->Lock(&dst, nullptr, nullptr), "Failed to lock AAC input buffer");
    std::memcpy(dst, pcm.data(), dataSize);
    PB_CHECK_HR(mediaBuffer->Unlock(), "Failed to unlock AAC input buffer");
    PB_CHECK_HR(mediaBuffer->SetCurrentLength(dataSize), "Failed to set AAC input length");

    Microsoft::WRL::ComPtr<IMFSample> sample;
    PB_CHECK_HR(MFCreateSample(&sample), "Failed to create AAC input sample");
    PB_CHECK_HR(sample->AddBuffer(mediaBuffer.Get()), "Failed to add AAC input buffer");
    PB_CHECK_HR(sample->SetSampleTime(relativeTs), "Failed to set AAC sample time");

    PB_CHECK_HR(sample->SetSampleDuration(duration), "Failed to set AAC sample duration");

    HRESULT hr = encoder_->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        if (!processOutput(packets)) {
            return false;
        }
        hr = encoder_->ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(hr)) {
        lastError_ = "AAC ProcessInput failed: " + hrToString(hr);
        return false;
    }

    return processOutput(packets);
}

bool AacMftEncoder::drain(std::vector<EncodedAudioPacket>& packets)
{
    if (!initialized_ || !encoder_) {
        return true;
    }

    encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    bool ok = true;
    for (;;) {
        const size_t before = packets.size();
        ok = processOutput(packets) && ok;
        if (packets.size() == before) {
            break;
        }
    }
    return ok;
}

bool AacMftEncoder::processOutput(std::vector<EncodedAudioPacket>& packets)
{
    MFT_OUTPUT_STREAM_INFO info = {};
    HRESULT hr = encoder_->GetOutputStreamInfo(0, &info);
    if (FAILED(hr)) {
        lastError_ = "AAC GetOutputStreamInfo failed: " + hrToString(hr);
        return false;
    }

    for (;;) {
        Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
        Microsoft::WRL::ComPtr<IMFSample> sample;
        if (!(info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
            PB_CHECK_HR(MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize, 16 * 1024), &buffer),
                        "Failed to create AAC output buffer");
            PB_CHECK_HR(MFCreateSample(&sample), "Failed to create AAC output sample");
            PB_CHECK_HR(sample->AddBuffer(buffer.Get()), "Failed to add AAC output buffer");
        }

        MFT_OUTPUT_DATA_BUFFER output = {};
        output.dwStreamID = 0;
        output.pSample = sample.Get();
        DWORD status = 0;
        hr = encoder_->ProcessOutput(0, 1, &output, &status);

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            return true;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (!configureOutputType()) {
                return false;
            }
            continue;
        }
        if (FAILED(hr)) {
            lastError_ = "AAC ProcessOutput failed: " + hrToString(hr);
            return false;
        }

        IMFSample* outSample = output.pSample ? output.pSample : sample.Get();
        const bool mftProvidedSample = output.pSample && output.pSample != sample.Get();
        if (!outSample) {
            return true;
        }

        Microsoft::WRL::ComPtr<IMFMediaBuffer> contiguous;
        PB_CHECK_HR(outSample->ConvertToContiguousBuffer(&contiguous),
                    "Failed to get AAC output buffer");
        BYTE* data = nullptr;
        DWORD length = 0;
        PB_CHECK_HR(contiguous->Lock(&data, nullptr, &length), "Failed to lock AAC output");

        if (length > 0) {
            LONGLONG sampleTime = 0;
            LONGLONG sampleDuration = 0;
            outSample->GetSampleTime(&sampleTime);
            outSample->GetSampleDuration(&sampleDuration);

            EncodedAudioPacket packet;
            packet.data.assign(data, data + length);
            packet.timestampMs = sampleTime / 10000;
            packet.durationMs = sampleDuration / 10000;
            packets.push_back(std::move(packet));
        }

        contiguous->Unlock();

        if (output.pEvents) {
            output.pEvents->Release();
        }
        if (mftProvidedSample) {
            output.pSample->Release();
        }

        if (output.dwStatus & MFT_OUTPUT_DATA_BUFFER_INCOMPLETE) {
            continue;
        }
    }
}

std::vector<int16_t> AacMftEncoder::convertToPcm16(const AudioBuffer& buffer) const
{
    const size_t sampleValues = static_cast<size_t>(buffer.sampleCount) * config_.audio.channelCount;
    std::vector<int16_t> pcm;
    pcm.reserve(sampleValues);

    if (buffer.bitsPerSample == 16) {
        const int16_t* src = reinterpret_cast<const int16_t*>(buffer.data.data());
        const size_t count = std::min(sampleValues, buffer.data.size() / sizeof(int16_t));
        pcm.insert(pcm.end(), src, src + count);
    } else if (buffer.bitsPerSample == 32) {
        const float* src = reinterpret_cast<const float*>(buffer.data.data());
        const size_t count = std::min(sampleValues, buffer.data.size() / sizeof(float));
        for (size_t i = 0; i < count; ++i) {
            pcm.push_back(floatToI16(src[i]));
        }
    } else {
        const uint8_t* src = buffer.data.data();
        for (size_t i = 0; i + 1 < buffer.data.size(); i += 2) {
            pcm.push_back(static_cast<int16_t>(src[i] | (src[i + 1] << 8)));
        }
    }

    return pcm;
}

void AacMftEncoder::shutdown()
{
    if (encoder_) {
        encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }
    encoder_.Reset();
    firstTimestamp_ = -1;
    nextInputTimestamp_ = -1;
    initialized_ = false;
}

} // namespace pb
