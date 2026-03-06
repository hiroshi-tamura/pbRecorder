#include "pipeline/MkvPipeline.h"
#include "pipeline/SinkWriterPipeline.h"

#include <matroska/KaxSegment.h>
#include <matroska/KaxTracks.h>
#include <matroska/KaxTrackEntryData.h>
#include <matroska/KaxTrackAudio.h>
#include <matroska/KaxTrackVideo.h>
#include <matroska/KaxCluster.h>
#include <matroska/KaxClusterData.h>
#include <matroska/KaxBlockData.h>
#include <matroska/KaxBlock.h>
#include <matroska/KaxCues.h>
#include <matroska/KaxCuesData.h>
#include <matroska/KaxInfo.h>
#include <matroska/KaxInfoData.h>
#include <matroska/KaxSeekHead.h>
#include <matroska/KaxVersion.h>
#include <ebml/EbmlHead.h>
#include <ebml/EbmlSubHead.h>
#include <ebml/EbmlVoid.h>
#include <ebml/StdIOCallback.h>

#include <opus.h>
#include <vorbis/codec.h>
#include <vorbis/vorbisenc.h>

#include <algorithm>
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <cassert>

using namespace libmatroska;
using namespace libebml;

namespace pb {

// ============================================================================
// Construction / Destruction
// ============================================================================

MkvPipeline::MkvPipeline() = default;

MkvPipeline::~MkvPipeline() {
    if (recording_) {
        stop();
    }
    releaseResources();
}

// ============================================================================
// IRecordingPipeline interface
// ============================================================================

bool MkvPipeline::initialize(const RecordingConfig& config, ID3D11Device* device) {
    if (initialized_) {
        reportError("MkvPipeline already initialized");
        return false;
    }

    config_ = config;
    d3dDevice_ = device;
    hasAudio_ = config_.recordAudio;

    // Determine if AAC audio goes through SinkWriter
    sinkWriterHasAudio_ = hasAudio_ && (config_.audio.codec == AudioCodec::AAC);

    // Generate temp .mp4 path from output path
    tempMp4Path_ = config_.outputPath;
    auto dotPos = tempMp4Path_.rfind(L'.');
    if (dotPos != std::wstring::npos)
        tempMp4Path_ = tempMp4Path_.substr(0, dotPos);
    tempMp4Path_ += L".tmp.mp4";

    // Create SinkWriterPipeline for H.264 encoding
    RecordingConfig swConfig = config_;
    swConfig.outputPath = tempMp4Path_;
    swConfig.container = ContainerFormat::MP4;
    swConfig.video.codec = VideoCodec::H264;
    swConfig.recordAudio = sinkWriterHasAudio_;
    // If AAC audio goes through SinkWriter, keep audio settings as-is

    sinkWriter_ = std::make_unique<SinkWriterPipeline>();
    sinkWriter_->setErrorCallback([this](const std::string& msg) {
        reportError("SinkWriter: " + msg);
    });

    if (!sinkWriter_->initialize(swConfig, device)) {
        reportError("Failed to initialize internal SinkWriter for H.264 encoding");
        return false;
    }

    // Initialize non-AAC audio encoder if needed
    if (hasAudio_ && !sinkWriterHasAudio_) {
        if (!initializeAudioEncoder()) {
            reportError("Failed to initialize audio encoder");
            return false;
        }
    }

    firstAudioTimestamp_ = -1;
    initialized_ = true;
    return true;
}

bool MkvPipeline::start() {
    if (!initialized_ || recording_) {
        return false;
    }

    if (!sinkWriter_->start()) {
        reportError("SinkWriter start failed");
        return false;
    }

    recording_ = true;
    return true;
}

bool MkvPipeline::stop() {
    if (!recording_) {
        return false;
    }
    recording_ = false;

    // Stop SinkWriter (finalizes temp .mp4)
    sinkWriter_->stop();

    // Remux temp .mp4 → final .mkv
    bool result = remuxToMkv();

    // Delete temp .mp4
    DeleteFileW(tempMp4Path_.c_str());

    return result;
}

bool MkvPipeline::writeVideoFrame(const VideoFrame& frame) {
    if (!recording_) {
        return false;
    }

    // Forward to SinkWriter for H.264 encoding
    return sinkWriter_->writeVideoFrame(frame);
}

bool MkvPipeline::writeAudioSamples(const AudioBuffer& buffer) {
    if (!recording_ || !hasAudio_) {
        return false;
    }

    // AAC goes through SinkWriter
    if (sinkWriterHasAudio_) {
        return sinkWriter_->writeAudioSamples(buffer);
    }

    // Non-AAC: encode and buffer
    int64_t expected = -1;
    firstAudioTimestamp_.compare_exchange_strong(expected, buffer.timestamp);

    int64_t relativeTimestamp = buffer.timestamp - firstAudioTimestamp_.load();
    int64_t tsMs = toMkvTimestamp(relativeTimestamp);

    std::vector<std::vector<uint8_t>> packets;

    {
        std::lock_guard<std::mutex> lock(encodeMutex_);

        switch (config_.audio.codec) {
        case AudioCodec::Opus:
            if (!encodeAudioOpus(buffer, packets)) return false;
            break;
        case AudioCodec::Vorbis:
            if (!encodeAudioVorbis(buffer, packets)) return false;
            break;
        case AudioCodec::PCM:
            packets.emplace_back(buffer.data.begin(), buffer.data.end());
            break;
        default:
            reportError("Unsupported audio codec for MKV");
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(audioBufferMutex_);
        for (auto& pkt : packets) {
            bufferedAudioPackets_.push_back({std::move(pkt), tsMs});
        }
    }

    return true;
}

void MkvPipeline::setErrorCallback(ErrorCallback callback) {
    errorCallback_ = std::move(callback);
}

bool MkvPipeline::isRecording() const {
    return recording_;
}

int64_t MkvPipeline::getDurationMs() const {
    if (sinkWriter_) {
        return sinkWriter_->getDurationMs();
    }
    return 0;
}

int64_t MkvPipeline::getFileSize() const {
    // During recording, report temp .mp4 size as estimate
    if (sinkWriter_) {
        return sinkWriter_->getFileSize();
    }
    return 0;
}

// ============================================================================
// Audio Encoder (Opus/Vorbis — AAC goes through SinkWriter)
// ============================================================================

bool MkvPipeline::initializeAudioEncoder() {
    switch (config_.audio.codec) {
    case AudioCodec::Opus:
        return initializeOpusEncoder();
    case AudioCodec::Vorbis:
        return initializeVorbisEncoder();
    case AudioCodec::PCM:
        return true; // No encoder needed
    default:
        reportError("Unsupported audio codec for MKV pipeline");
        return false;
    }
}

bool MkvPipeline::initializeOpusEncoder() {
    int err = 0;
    opusEncoder_ = opus_encoder_create(
        config_.audio.sampleRate,
        config_.audio.channelCount,
        OPUS_APPLICATION_AUDIO,
        &err);

    if (err != OPUS_OK || !opusEncoder_) {
        reportError("opus_encoder_create failed: " + std::to_string(err));
        return false;
    }

    opus_encoder_ctl(opusEncoder_, OPUS_SET_BITRATE(config_.audio.bitrate));
    opus_encoder_ctl(opusEncoder_, OPUS_SET_COMPLEXITY(10));
    opus_encoder_ctl(opusEncoder_, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));

    // 20ms frames at configured sample rate
    opusFrameSamples_ = config_.audio.sampleRate / 50;
    opusResidualBuffer_.clear();

    return true;
}

bool MkvPipeline::initializeVorbisEncoder() {
    vorbisInfo_ = new vorbis_info;
    vorbis_info_init(vorbisInfo_);

    int ret = vorbis_encode_init(vorbisInfo_,
                                  config_.audio.channelCount,
                                  config_.audio.sampleRate,
                                  -1, config_.audio.bitrate, -1);
    if (ret != 0) {
        reportError("vorbis_encode_init failed: " + std::to_string(ret));
        vorbis_info_clear(vorbisInfo_);
        delete vorbisInfo_;
        vorbisInfo_ = nullptr;
        return false;
    }

    vorbisComment_ = new vorbis_comment;
    vorbis_comment_init(vorbisComment_);
    vorbis_comment_add_tag(vorbisComment_, "ENCODER", "pbRecorder");

    vorbisDsp_ = new vorbis_dsp_state;
    vorbis_analysis_init(vorbisDsp_, vorbisInfo_);

    vorbisBlock_ = new vorbis_block;
    vorbis_block_init(vorbisDsp_, vorbisBlock_);

    ogg_packet header, headerComment, headerCodebook;
    vorbis_analysis_headerout(vorbisDsp_, vorbisComment_,
                               &header, &headerComment, &headerCodebook);

    vorbisHeaders_.clear();
    vorbisHeaders_.emplace_back(header.packet, header.packet + header.bytes);
    vorbisHeaders_.emplace_back(headerComment.packet,
                                 headerComment.packet + headerComment.bytes);
    vorbisHeaders_.emplace_back(headerCodebook.packet,
                                 headerCodebook.packet + headerCodebook.bytes);

    return true;
}

bool MkvPipeline::encodeAudioOpus(const AudioBuffer& buffer,
                                   std::vector<std::vector<uint8_t>>& packets) {
    int channels = config_.audio.channelCount;
    int bps = config_.audio.bitsPerSample;
    int totalSamples = static_cast<int>(buffer.sampleCount);

    std::vector<float> inputFloat;
    inputFloat.reserve(opusResidualBuffer_.size() + totalSamples * channels);

    inputFloat.insert(inputFloat.end(),
                      opusResidualBuffer_.begin(),
                      opusResidualBuffer_.end());
    opusResidualBuffer_.clear();

    if (bps == 16) {
        const int16_t* src = reinterpret_cast<const int16_t*>(buffer.data.data());
        for (int i = 0; i < totalSamples * channels; ++i) {
            inputFloat.push_back(static_cast<float>(src[i]) / 32768.0f);
        }
    } else if (bps == 32) {
        const float* src = reinterpret_cast<const float*>(buffer.data.data());
        inputFloat.insert(inputFloat.end(), src, src + totalSamples * channels);
    } else {
        const int16_t* src = reinterpret_cast<const int16_t*>(buffer.data.data());
        int count = static_cast<int>(buffer.data.size()) / 2;
        for (int i = 0; i < count; ++i) {
            inputFloat.push_back(static_cast<float>(src[i]) / 32768.0f);
        }
    }

    int frameSamplesTotal = opusFrameSamples_ * channels;
    int pos = 0;
    int totalFloats = static_cast<int>(inputFloat.size());

    std::vector<uint8_t> encodedBuf(4000);

    while (pos + frameSamplesTotal <= totalFloats) {
        int encoded = opus_encode_float(
            opusEncoder_,
            inputFloat.data() + pos,
            opusFrameSamples_,
            encodedBuf.data(),
            static_cast<opus_int32>(encodedBuf.size()));

        if (encoded < 0) {
            reportError("opus_encode_float error: " + std::to_string(encoded));
            return false;
        }

        packets.emplace_back(encodedBuf.data(), encodedBuf.data() + encoded);
        pos += frameSamplesTotal;
    }

    if (pos < totalFloats) {
        opusResidualBuffer_.assign(inputFloat.begin() + pos, inputFloat.end());
    }

    return true;
}

bool MkvPipeline::encodeAudioVorbis(const AudioBuffer& buffer,
                                     std::vector<std::vector<uint8_t>>& packets) {
    int channels = config_.audio.channelCount;
    int totalSamples = static_cast<int>(buffer.sampleCount);

    float** vorbisBuffer = vorbis_analysis_buffer(vorbisDsp_, totalSamples);

    if (config_.audio.bitsPerSample == 16) {
        const int16_t* src = reinterpret_cast<const int16_t*>(buffer.data.data());
        for (int s = 0; s < totalSamples; ++s) {
            for (int c = 0; c < channels; ++c) {
                vorbisBuffer[c][s] =
                    static_cast<float>(src[s * channels + c]) / 32768.0f;
            }
        }
    } else if (config_.audio.bitsPerSample == 32) {
        const float* src = reinterpret_cast<const float*>(buffer.data.data());
        for (int s = 0; s < totalSamples; ++s) {
            for (int c = 0; c < channels; ++c) {
                vorbisBuffer[c][s] = src[s * channels + c];
            }
        }
    }

    vorbis_analysis_wrote(vorbisDsp_, totalSamples);

    while (vorbis_analysis_blockout(vorbisDsp_, vorbisBlock_) == 1) {
        vorbis_analysis(vorbisBlock_, nullptr);
        vorbis_bitrate_addblock(vorbisBlock_);

        ogg_packet op;
        while (vorbis_bitrate_flushpacket(vorbisDsp_, &op)) {
            packets.emplace_back(op.packet, op.packet + op.bytes);
        }
    }

    return true;
}

void MkvPipeline::shutdownAudioEncoder() {
    if (opusEncoder_) {
        opus_encoder_destroy(opusEncoder_);
        opusEncoder_ = nullptr;
    }

    if (vorbisBlock_) {
        vorbis_block_clear(vorbisBlock_);
        delete vorbisBlock_;
        vorbisBlock_ = nullptr;
    }
    if (vorbisDsp_) {
        vorbis_dsp_clear(vorbisDsp_);
        delete vorbisDsp_;
        vorbisDsp_ = nullptr;
    }
    if (vorbisComment_) {
        vorbis_comment_clear(vorbisComment_);
        delete vorbisComment_;
        vorbisComment_ = nullptr;
    }
    if (vorbisInfo_) {
        vorbis_info_clear(vorbisInfo_);
        delete vorbisInfo_;
        vorbisInfo_ = nullptr;
    }
    vorbisHeaders_.clear();
    opusResidualBuffer_.clear();
}

// ============================================================================
// Remux: temp .mp4 → .mkv
// ============================================================================

bool MkvPipeline::remuxToMkv() {
    // Start MF for source reader (reference counted, safe to call again)
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        reportError("MFStartup failed for remux: " + hrToString(hr));
        return false;
    }

    bool success = false;

    // Debug log file
    std::string logPath;
    {
        const std::wstring& wp = config_.outputPath;
        int n = WideCharToMultiByte(CP_UTF8, 0, wp.c_str(), (int)wp.size(), nullptr, 0, nullptr, nullptr);
        logPath.resize(n);
        WideCharToMultiByte(CP_UTF8, 0, wp.c_str(), (int)wp.size(), &logPath[0], n, nullptr, nullptr);
        logPath += ".remux.log";
    }
    FILE* logFile = fopen(logPath.c_str(), "w");
    auto logMsg = [&](const char* fmt, ...) {
        if (!logFile) return;
        va_list args;
        va_start(args, fmt);
        vfprintf(logFile, fmt, args);
        va_end(args);
        fprintf(logFile, "\n");
        fflush(logFile);
    };

    logMsg("=== Remux Start ===");

    // Open temp .mp4 with source reader
    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFAttributes> readerAttrs;
    MFCreateAttributes(&readerAttrs, 1);
    // Disable converters so we get native compressed samples
    readerAttrs->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);

    hr = MFCreateSourceReaderFromURL(tempMp4Path_.c_str(), readerAttrs.Get(), &reader);
    if (FAILED(hr)) {
        reportError("Failed to open temp .mp4 for remux: " + hrToString(hr));
        logMsg("MFCreateSourceReaderFromURL failed: 0x%08lX", (unsigned long)hr);
        if (logFile) fclose(logFile);
        MFShutdown();
        return false;
    }
    logMsg("Source reader created OK");

    // Get native video type info
    ComPtr<IMFMediaType> nativeVideoType;
    hr = reader->GetNativeMediaType(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &nativeVideoType);
    if (FAILED(hr)) {
        reportError("Failed to get video type from temp .mp4: " + hrToString(hr));
        logMsg("GetNativeMediaType failed: 0x%08lX", (unsigned long)hr);
        if (logFile) fclose(logFile);
        MFShutdown();
        return false;
    }

    // Log native type subtype
    GUID subtype = {};
    nativeVideoType->GetGUID(MF_MT_SUBTYPE, &subtype);
    logMsg("Native video subtype: %08lX-%04X-%04X-%02X%02X%02X%02X%02X%02X%02X%02X",
           subtype.Data1, subtype.Data2, subtype.Data3,
           subtype.Data4[0], subtype.Data4[1], subtype.Data4[2], subtype.Data4[3],
           subtype.Data4[4], subtype.Data4[5], subtype.Data4[6], subtype.Data4[7]);

    // Check for MPEG sequence header (codec config data)
    UINT32 seqHeaderSize = 0;
    hr = nativeVideoType->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &seqHeaderSize);
    logMsg("MF_MT_MPEG_SEQUENCE_HEADER: hr=0x%08lX size=%u", (unsigned long)hr, seqHeaderSize);

    // Try MF_MT_USER_DATA too
    UINT32 userDataSize = 0;
    hr = nativeVideoType->GetBlobSize(MF_MT_USER_DATA, &userDataSize);
    logMsg("MF_MT_USER_DATA: hr=0x%08lX size=%u", (unsigned long)hr, userDataSize);

    // If we have sequence header, try to extract SPS/PPS from it
    if (seqHeaderSize > 0) {
        std::vector<uint8_t> seqHeader(seqHeaderSize);
        nativeVideoType->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, seqHeader.data(),
                                  seqHeaderSize, &seqHeaderSize);
        logMsg("Sequence header first bytes: %02X %02X %02X %02X %02X",
               seqHeader.size() > 0 ? seqHeader[0] : 0,
               seqHeader.size() > 1 ? seqHeader[1] : 0,
               seqHeader.size() > 2 ? seqHeader[2] : 0,
               seqHeader.size() > 3 ? seqHeader[3] : 0,
               seqHeader.size() > 4 ? seqHeader[4] : 0);

        // Check format: if starts with 0x01, it's AVCDecoderConfigurationRecord
        // If starts with 0x00 0x00 0x01, it's Annex B (SPS/PPS with start codes)
        bool isAnnexBHeader = (seqHeader.size() >= 4 &&
            seqHeader[0] == 0x00 && seqHeader[1] == 0x00 &&
            (seqHeader[2] == 0x01 || (seqHeader[2] == 0x00 && seqHeader[3] == 0x01)));

        if (isAnnexBHeader) {
            logMsg("Sequence header is Annex B format, extracting SPS/PPS");
            extractSpsPps(seqHeader.data(), seqHeader.size());
            logMsg("SPS/PPS from Annex B seq header: sps=%zu pps=%zu extracted=%d",
                   spsData_.size(), ppsData_.size(), spsPpsExtracted_ ? 1 : 0);
        } else if (seqHeader.size() > 6 && seqHeader[0] == 0x01) {
            logMsg("Found AVCDecoderConfigurationRecord in sequence header");
            // Parse SPS/PPS from the avcC record
            // Format: version(1) + profile(1) + compat(1) + level(1) + lengthSizeMinusOne(1)
            //         + numSPS(1) + spsLen(2) + spsData + numPPS(1) + ppsLen(2) + ppsData
            size_t offset = 5;
            uint8_t numSps = seqHeader[offset] & 0x1F;
            offset++;
            for (int i = 0; i < numSps && offset + 2 <= seqHeader.size(); ++i) {
                uint16_t spsLen = (seqHeader[offset] << 8) | seqHeader[offset + 1];
                offset += 2;
                if (offset + spsLen <= seqHeader.size()) {
                    spsData_.assign(seqHeader.begin() + offset, seqHeader.begin() + offset + spsLen);
                    offset += spsLen;
                }
            }
            if (offset < seqHeader.size()) {
                uint8_t numPps = seqHeader[offset];
                offset++;
                for (int i = 0; i < numPps && offset + 2 <= seqHeader.size(); ++i) {
                    uint16_t ppsLen = (seqHeader[offset] << 8) | seqHeader[offset + 1];
                    offset += 2;
                    if (offset + ppsLen <= seqHeader.size()) {
                        ppsData_.assign(seqHeader.begin() + offset, seqHeader.begin() + offset + ppsLen);
                        offset += ppsLen;
                    }
                }
            }
            spsPpsExtracted_ = !spsData_.empty() && !ppsData_.empty();
            logMsg("SPS/PPS from avcC: sps=%zu bytes, pps=%zu bytes, extracted=%d",
                   spsData_.size(), ppsData_.size(), spsPpsExtracted_ ? 1 : 0);
        }
    }

    // If AAC audio is in the .mp4, check native audio type
    if (sinkWriterHasAudio_) {
        ComPtr<IMFMediaType> nativeAudioType;
        hr = reader->GetNativeMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, &nativeAudioType);
        if (SUCCEEDED(hr)) {
            GUID audioSubtype = {};
            nativeAudioType->GetGUID(MF_MT_SUBTYPE, &audioSubtype);
            logMsg("Native audio subtype: %08lX", audioSubtype.Data1);
        } else {
            logMsg("GetNativeMediaType audio failed: 0x%08lX", (unsigned long)hr);
        }
    }

    // Initialize MKV writer (creates the output .mkv file)
    if (!initializeMkvWriter()) {
        reportError("Failed to initialize MKV writer during remux");
        logMsg("initializeMkvWriter failed");
        if (logFile) fclose(logFile);
        MFShutdown();
        return false;
    }
    logMsg("MKV writer initialized OK");

    // Streaming merge: read video and audio samples alternately by timestamp
    // For non-AAC audio, use buffered packets
    std::sort(bufferedAudioPackets_.begin(), bufferedAudioPackets_.end(),
              [](const AudioPacket& a, const AudioPacket& b) {
                  return a.timestampMs < b.timestampMs;
              });
    size_t audioPacketIdx = 0;

    // Pre-read first video sample
    struct PendingSample {
        std::vector<uint8_t> data;
        int64_t timestampMs = 0;
        bool keyframe = false;
        bool valid = false;
    };

    int videoSampleCount = 0;
    int audioSampleCount = 0;

    auto readNextVideo = [&](PendingSample& out) {
        out.valid = false;
        ComPtr<IMFSample> sample;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        HRESULT readHr = reader->ReadSample(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
            0, nullptr, &flags, &timestamp, &sample);
        if (FAILED(readHr)) {
            logMsg("ReadSample video failed: 0x%08lX (sample #%d)", (unsigned long)readHr, videoSampleCount);
            return;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            logMsg("Video EOS after %d samples", videoSampleCount);
            return;
        }
        if (!sample) {
            logMsg("Video sample is null (flags=0x%08lX)", flags);
            return;
        }

        ComPtr<IMFMediaBuffer> buf;
        sample->ConvertToContiguousBuffer(&buf);
        BYTE* pData = nullptr;
        DWORD len = 0;
        buf->Lock(&pData, nullptr, &len);
        out.data.assign(pData, pData + len);
        buf->Unlock();

        out.timestampMs = timestamp / 10000; // 100ns → ms
        UINT32 isKey = 0;
        sample->GetUINT32(MFSampleExtension_CleanPoint, &isKey);
        out.keyframe = isKey != 0;
        out.valid = true;
        ++videoSampleCount;

        // Log first few samples for debugging
        if (videoSampleCount <= 5) {
            logMsg("Video sample #%d: ts=%lldms len=%lu key=%d first4bytes=%02X%02X%02X%02X",
                   videoSampleCount, (long long)out.timestampMs, (unsigned long)len,
                   out.keyframe ? 1 : 0,
                   len > 0 ? pData[0] : 0, len > 1 ? pData[1] : 0,
                   len > 2 ? pData[2] : 0, len > 3 ? pData[3] : 0);
        }
    };

    auto readNextAacAudio = [&](PendingSample& out) {
        out.valid = false;
        if (!sinkWriterHasAudio_) return;

        ComPtr<IMFSample> sample;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        HRESULT readHr = reader->ReadSample(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM),
            0, nullptr, &flags, &timestamp, &sample);
        if (FAILED(readHr)) {
            logMsg("ReadSample audio failed: 0x%08lX (sample #%d)", (unsigned long)readHr, audioSampleCount);
            return;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            logMsg("Audio EOS after %d samples", audioSampleCount);
            return;
        }
        if (!sample) {
            logMsg("Audio sample is null (flags=0x%08lX)", flags);
            return;
        }

        ComPtr<IMFMediaBuffer> buf;
        sample->ConvertToContiguousBuffer(&buf);
        BYTE* pData = nullptr;
        DWORD len = 0;
        buf->Lock(&pData, nullptr, &len);
        out.data.assign(pData, pData + len);
        buf->Unlock();

        out.timestampMs = timestamp / 10000;
        out.keyframe = false;
        out.valid = true;
        ++audioSampleCount;

        if (audioSampleCount <= 3) {
            logMsg("Audio sample #%d: ts=%lldms len=%lu", audioSampleCount,
                   (long long)out.timestampMs, (unsigned long)len);
        }
    };

    auto readNextBufferedAudio = [&](PendingSample& out) {
        out.valid = false;
        if (audioPacketIdx >= bufferedAudioPackets_.size()) return;
        auto& pkt = bufferedAudioPackets_[audioPacketIdx];
        out.data = std::move(pkt.data);
        out.timestampMs = pkt.timestampMs;
        out.keyframe = false;
        out.valid = true;
        ++audioPacketIdx;
    };

    PendingSample videoSample, audioSample;
    readNextVideo(videoSample);

    if (sinkWriterHasAudio_) {
        readNextAacAudio(audioSample);
    } else if (hasAudio_) {
        readNextBufferedAudio(audioSample);
    }

    while (videoSample.valid || audioSample.valid) {
        bool writeVideo = false;
        if (videoSample.valid && audioSample.valid) {
            writeVideo = (videoSample.timestampMs <= audioSample.timestampMs);
        } else {
            writeVideo = videoSample.valid;
        }

        if (writeVideo) {
            // Extract SPS/PPS from first keyframe
            if (videoSample.keyframe && !spsPpsExtracted_) {
                if (extractSpsPps(videoSample.data.data(), videoSample.data.size())) {
                    patchVideoCodecPrivate();
                }
            }

            writeClusterData(VIDEO_TRACK_NUM, videoSample.timestampMs,
                             videoSample.data.data(), videoSample.data.size(),
                             videoSample.keyframe);
            readNextVideo(videoSample);
        } else {
            writeClusterData(AUDIO_TRACK_NUM, audioSample.timestampMs,
                             audioSample.data.data(), audioSample.data.size(),
                             false);

            if (sinkWriterHasAudio_) {
                readNextAacAudio(audioSample);
            } else {
                readNextBufferedAudio(audioSample);
            }
        }
    }

    bufferedAudioPackets_.clear();

    logMsg("Remux complete: %d video samples, %d audio samples written",
           videoSampleCount, audioSampleCount);
    logMsg("SPS/PPS extracted: %d (sps=%zu pps=%zu)",
           spsPpsExtracted_ ? 1 : 0, spsData_.size(), ppsData_.size());

    // Patch video codec private if we got SPS/PPS from avcC but not yet patched
    if (spsPpsExtracted_) {
        patchVideoCodecPrivate();
    }

    finalizeMkvFile();

    reader.Reset();
    MFShutdown();

    if (logFile) fclose(logFile);

    success = true;
    return success;
}

// ============================================================================
// MKV Muxing (libmatroska / libebml)
// ============================================================================

bool MkvPipeline::initializeMkvWriter() {
    // Convert wide string path to narrow for StdIOCallback
    std::string narrowPath;
    {
        const std::wstring& wpath = config_.outputPath;
        int needed = WideCharToMultiByte(CP_UTF8, 0,
                                          wpath.c_str(),
                                          static_cast<int>(wpath.size()),
                                          nullptr, 0, nullptr, nullptr);
        narrowPath.resize(needed);
        WideCharToMultiByte(CP_UTF8, 0,
                            wpath.c_str(),
                            static_cast<int>(wpath.size()),
                            &narrowPath[0], needed, nullptr, nullptr);
    }

    try {
        mkvFile_ = std::make_unique<StdIOCallback>(
            narrowPath.c_str(), ::MODE_CREATE);
    } catch (const std::exception& e) {
        reportError(std::string("Failed to create MKV file: ") + e.what());
        return false;
    }

    if (!writeEbmlHeader()) return false;

    // Create segment
    segment_ = std::make_unique<KaxSegment>();

    // Write segment header manually: ID(4 bytes) + unknown size(8 bytes)
    {
        uint8_t segHdr[] = {
            0x18, 0x53, 0x80, 0x67,
            0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
        };
        segmentSizeOffset_ = mkvFile_->getFilePointer() + 4;
        mkvFile_->write(segHdr, sizeof(segHdr));
    }
    segmentDataStart_ = mkvFile_->getFilePointer();

    // Reserve space for SeekHead
    {
        EbmlVoid placeholder;
        placeholder.SetSize(256);
        seekHeadPlaceholderPos_ = mkvFile_->getFilePointer();
        placeholder.Render(*mkvFile_);
        seekHeadPlaceholderSize_ = mkvFile_->getFilePointer() -
                                    seekHeadPlaceholderPos_;
    }

    if (!writeSegmentInfo()) return false;
    if (!writeTracks()) return false;

    cues_ = std::make_unique<KaxCues>();

    return true;
}

bool MkvPipeline::writeEbmlHeader() {
    std::vector<uint8_t> hdr;

    auto writeEbmlId = [&](uint32_t id, int len) {
        for (int i = len - 1; i >= 0; --i)
            hdr.push_back(static_cast<uint8_t>((id >> (i * 8)) & 0xFF));
    };
    auto writeVint = [&](uint64_t val, int len) {
        uint8_t marker = static_cast<uint8_t>(1 << (8 - len));
        uint8_t firstByte = marker | static_cast<uint8_t>((val >> ((len - 1) * 8)) & (marker - 1));
        hdr.push_back(firstByte);
        for (int i = len - 2; i >= 0; --i)
            hdr.push_back(static_cast<uint8_t>((val >> (i * 8)) & 0xFF));
    };
    auto writeUintElement = [&](uint32_t id, int idLen, uint64_t val) {
        writeEbmlId(id, idLen);
        writeVint(1, 1);
        hdr.push_back(static_cast<uint8_t>(val));
    };
    auto writeStringElement = [&](uint32_t id, int idLen, const std::string& s) {
        writeEbmlId(id, idLen);
        writeVint(s.size(), 1);
        hdr.insert(hdr.end(), s.begin(), s.end());
    };

    writeEbmlId(0x1A45DFA3, 4);
    writeVint(35, 1);

    writeUintElement(0x4286, 2, 1);
    writeUintElement(0x42F7, 2, 1);
    writeUintElement(0x42F2, 2, 4);
    writeUintElement(0x42F3, 2, 8);
    writeStringElement(0x4282, 2, "matroska");
    writeUintElement(0x4287, 2, 4);
    writeUintElement(0x4285, 2, 2);

    mkvFile_->write(hdr.data(), hdr.size());
    return true;
}

bool MkvPipeline::writeSegmentInfo() {
    std::vector<uint8_t> buf;

    auto appendEbmlId = [&](uint32_t id, int len) {
        for (int i = len - 1; i >= 0; --i)
            buf.push_back(static_cast<uint8_t>((id >> (i * 8)) & 0xFF));
    };
    auto appendVint = [&](uint64_t val, int len) {
        uint8_t marker = static_cast<uint8_t>(1 << (8 - len));
        uint8_t first = marker | static_cast<uint8_t>((val >> ((len - 1) * 8)) & (marker - 1));
        buf.push_back(first);
        for (int i = len - 2; i >= 0; --i)
            buf.push_back(static_cast<uint8_t>((val >> (i * 8)) & 0xFF));
    };
    auto appendUintElement = [&](uint32_t id, int idLen, uint64_t val, int valLen) {
        appendEbmlId(id, idLen);
        appendVint(valLen, 1);
        for (int i = valLen - 1; i >= 0; --i)
            buf.push_back(static_cast<uint8_t>((val >> (i * 8)) & 0xFF));
    };
    auto appendUtf8Element = [&](uint32_t id, int idLen, const std::string& s) {
        appendEbmlId(id, idLen);
        appendVint(s.size(), 1);
        buf.insert(buf.end(), s.begin(), s.end());
    };

    appendUintElement(0x2AD7B1, 3, 1000000, 3);
    appendUtf8Element(0x4D80, 2, "pbRecorder libmatroska");
    appendUtf8Element(0x5741, 2, "pbRecorder");

    // Duration placeholder (float64 = 8 bytes): ID 0x4489 + size 0x88 + 8 bytes
    // Will be patched in finalizeMkvFile
    size_t durationOffsetInBuf = buf.size();
    appendEbmlId(0x4489, 2);
    appendVint(8, 1);
    for (int i = 0; i < 8; ++i) buf.push_back(0); // placeholder zero duration

    std::vector<uint8_t> infoData;
    std::swap(buf, infoData);

    segmentInfoPos_ = mkvFile_->getFilePointer();
    uint8_t infoId[] = {0x15, 0x49, 0xA9, 0x66};
    mkvFile_->write(infoId, 4);

    uint64_t infoSize = infoData.size();
    uint8_t sizeVint[2] = {
        static_cast<uint8_t>(0x40 | ((infoSize >> 8) & 0x3F)),
        static_cast<uint8_t>(infoSize & 0xFF)
    };
    mkvFile_->write(sizeVint, 2);

    // Record absolute position of the Duration float data (after ID + size byte)
    durationPlaceholderPos_ = segmentInfoPos_ + 4 + 2 + durationOffsetInBuf + 2 + 1;

    mkvFile_->write(infoData.data(), infoData.size());

    return true;
}

bool MkvPipeline::writeTracks() {
    struct EbmlBuilder {
        std::vector<uint8_t> buf;

        void writeId(uint32_t id, int len) {
            for (int i = len - 1; i >= 0; --i)
                buf.push_back(static_cast<uint8_t>((id >> (i * 8)) & 0xFF));
        }
        void writeVint(uint64_t val, int len) {
            uint8_t marker = static_cast<uint8_t>(1 << (8 - len));
            uint8_t first = marker | static_cast<uint8_t>((val >> ((len - 1) * 8)) & (marker - 1));
            buf.push_back(first);
            for (int i = len - 2; i >= 0; --i)
                buf.push_back(static_cast<uint8_t>((val >> (i * 8)) & 0xFF));
        }
        void writeUint(uint32_t id, int idLen, uint64_t val, int valLen) {
            writeId(id, idLen);
            writeVint(valLen, 1);
            for (int i = valLen - 1; i >= 0; --i)
                buf.push_back(static_cast<uint8_t>((val >> (i * 8)) & 0xFF));
        }
        void writeString(uint32_t id, int idLen, const std::string& s) {
            writeId(id, idLen);
            writeVint(s.size(), 1);
            buf.insert(buf.end(), s.begin(), s.end());
        }
        void writeFloat(uint32_t id, int idLen, double val) {
            writeId(id, idLen);
            writeVint(8, 1);
            uint64_t bits;
            std::memcpy(&bits, &val, 8);
            for (int i = 7; i >= 0; --i)
                buf.push_back(static_cast<uint8_t>((bits >> (i * 8)) & 0xFF));
        }
        void writeBinary(uint32_t id, int idLen, const uint8_t* data, size_t size) {
            writeId(id, idLen);
            if (size < 128) {
                writeVint(size, 1);
            } else {
                writeVint(size, 2);
            }
            buf.insert(buf.end(), data, data + size);
        }
        void writeElement(uint32_t id, int idLen, const std::vector<uint8_t>& data) {
            writeId(id, idLen);
            size_t size = data.size();
            if (size < 128) {
                writeVint(size, 1);
            } else if (size < 16384) {
                writeVint(size, 2);
            } else {
                writeVint(size, 3);
            }
            buf.insert(buf.end(), data.begin(), data.end());
        }
    };

    // ---- Build video track entry ----
    EbmlBuilder videoTrack;
    videoTrack.writeUint(0xD7, 1, VIDEO_TRACK_NUM, 1);
    videoTrack.writeUint(0x73C5, 2, 1, 1);
    videoTrack.writeUint(0x83, 1, 1, 1);
    videoTrack.writeUint(0x9C, 1, 0, 1);
    uint64_t defaultDuration = 1000000000ULL / config_.video.fps;
    videoTrack.writeUint(0x23E383, 3, defaultDuration, 4);
    videoTrack.writeString(0x86, 1, "V_MPEG4/ISO/AVC");

    // CodecPrivate placeholder: reserve 128 bytes as EbmlVoid
    size_t codecPrivateVoidOffset = videoTrack.buf.size();
    videoTrack.buf.push_back(0xEC);
    videoTrack.buf.push_back(0xFE);
    videoTrack.buf.resize(videoTrack.buf.size() + 126, 0x00);

    EbmlBuilder videoSettings;
    videoSettings.writeUint(0xB0, 1, config_.video.width, 2);
    videoSettings.writeUint(0xBA, 1, config_.video.height, 2);
    videoSettings.writeUint(0x54B0, 2, config_.video.width, 2);
    videoSettings.writeUint(0x54BA, 2, config_.video.height, 2);
    videoTrack.writeElement(0xE0, 1, videoSettings.buf);

    // ---- Build audio track entry (if needed) ----
    EbmlBuilder audioTrack;
    if (hasAudio_) {
        audioTrack.writeUint(0xD7, 1, AUDIO_TRACK_NUM, 1);
        audioTrack.writeUint(0x73C5, 2, 2, 1);
        audioTrack.writeUint(0x83, 1, 2, 1);
        audioTrack.writeUint(0x9C, 1, 1, 1);

        std::string audioCodecId;
        switch (config_.audio.codec) {
        case AudioCodec::AAC:    audioCodecId = "A_AAC"; break;
        case AudioCodec::Opus:   audioCodecId = "A_OPUS"; break;
        case AudioCodec::Vorbis: audioCodecId = "A_VORBIS"; break;
        case AudioCodec::PCM:    audioCodecId = "A_PCM/INT/LIT"; break;
        default:                 audioCodecId = "A_AAC"; break;
        }
        audioTrack.writeString(0x86, 1, audioCodecId);

        std::vector<uint8_t> audioCodecPrivate;
        switch (config_.audio.codec) {
        case AudioCodec::AAC:    audioCodecPrivate = buildAudioSpecificConfig(); break;
        case AudioCodec::Opus:   audioCodecPrivate = buildOpusHead(); break;
        case AudioCodec::Vorbis: audioCodecPrivate = buildVorbisCodecPrivate(); break;
        default: break;
        }
        if (!audioCodecPrivate.empty()) {
            audioTrack.writeBinary(0x63A2, 2, audioCodecPrivate.data(),
                                   audioCodecPrivate.size());
        }

        EbmlBuilder audioSettings;
        audioSettings.writeFloat(0xB5, 1,
            static_cast<double>(config_.audio.sampleRate));
        audioSettings.writeUint(0x9F, 1, config_.audio.channelCount, 1);
        audioSettings.writeUint(0x6264, 2, config_.audio.bitsPerSample, 1);
        audioTrack.writeElement(0xE1, 1, audioSettings.buf);
    }

    // ---- Assemble TrackEntry elements ----
    EbmlBuilder tracksBody;
    tracksBody.writeElement(0xAE, 1, videoTrack.buf);

    if (hasAudio_) {
        tracksBody.writeElement(0xAE, 1, audioTrack.buf);
    }

    // ---- Write Tracks element ----
    uint64_t tracksPos = mkvFile_->getFilePointer();
    uint8_t tracksId[] = {0x16, 0x54, 0xAE, 0x6B};
    mkvFile_->write(tracksId, 4);

    uint64_t tracksSize = tracksBody.buf.size();
    uint8_t tSizeVint[4] = {
        static_cast<uint8_t>(0x10 | ((tracksSize >> 24) & 0x0F)),
        static_cast<uint8_t>((tracksSize >> 16) & 0xFF),
        static_cast<uint8_t>((tracksSize >> 8) & 0xFF),
        static_cast<uint8_t>(tracksSize & 0xFF)
    };
    mkvFile_->write(tSizeVint, 4);

    size_t trackEntryHeaderSize = 1;
    if (videoTrack.buf.size() < 128) trackEntryHeaderSize += 1;
    else if (videoTrack.buf.size() < 16384) trackEntryHeaderSize += 2;
    else trackEntryHeaderSize += 3;

    codecPrivatePlaceholderPos_ = tracksPos + 4 + 4 + trackEntryHeaderSize +
                                   codecPrivateVoidOffset;
    codecPrivatePlaceholderSize_ = 128;

    mkvFile_->write(tracksBody.buf.data(), tracksBody.buf.size());

    auto& libTracks = GetChild<KaxTracks>(*segment_);
    auto& vTrack = GetChild<KaxTrackEntry>(libTracks);
    GetChild<KaxTrackNumber>(vTrack).SetValue(VIDEO_TRACK_NUM);
    videoTrackEntry_ = &vTrack;

    if (hasAudio_) {
        auto& aTrack = AddNewChild<KaxTrackEntry>(libTracks);
        GetChild<KaxTrackNumber>(aTrack).SetValue(AUDIO_TRACK_NUM);
        audioTrackEntry_ = &aTrack;
    }

    return true;
}

void MkvPipeline::writeClusterData(uint8_t trackNum, int64_t timestampMs,
                                    const uint8_t* data, size_t size,
                                    bool keyframe) {
    if (!mkvFile_ || !segment_) return;

    // For video track, convert Annex B → AVCC format if needed
    std::vector<uint8_t> avccData;
    const uint8_t* frameData = data;
    size_t frameSize = size;

    if (trackNum == VIDEO_TRACK_NUM) {
        // Auto-detect: check if data is in Annex B format (starts with start code)
        bool isAnnexB = false;
        if (size >= 4 && data[0] == 0 && data[1] == 0) {
            if (data[2] == 0 && data[3] == 1) isAnnexB = true;
            else if (data[2] == 1) isAnnexB = true;
        }

        if (isAnnexB) {
            avccData = annexBToAvcc(data, size);
            if (avccData.empty()) return;
            frameData = avccData.data();
            frameSize = avccData.size();
        }
        // else: already in AVCC format, use as-is
    }

    if (timestampMs > maxTimestampMs_) maxTimestampMs_ = timestampMs;

    // Start a new cluster if needed
    bool needNewCluster = (currentCluster_ == nullptr) ||
                          (timestampMs - clusterStartTimestamp_ >= CLUSTER_DURATION_MS);

    if (needNewCluster) {
        flushCurrentCluster();

        currentCluster_ = new KaxCluster();
        currentCluster_->SetParent(*segment_);
        currentCluster_->InitTimecode(timestampMs, 1000000);
        clusterStartTimestamp_ = timestampMs;

        if (keyframe && trackNum == VIDEO_TRACK_NUM) {
            CueEntry cue;
            cue.timestampMs = timestampMs;
            cue.trackNum = trackNum;
            cue.clusterPosition = mkvFile_->getFilePointer() - segmentDataStart_;
            cueEntries_.push_back(cue);
        }
    } else if (keyframe && trackNum == VIDEO_TRACK_NUM) {
        CueEntry cue;
        cue.timestampMs = timestampMs;
        cue.trackNum = trackNum;
        cue.clusterPosition = lastClusterTimestamp_;
        cueEntries_.push_back(cue);
    }

    KaxSimpleBlock& simpleBlock = AddNewChild<KaxSimpleBlock>(*currentCluster_);

    auto* dataCopy = static_cast<uint8*>(std::malloc(frameSize));
    std::memcpy(dataCopy, frameData, frameSize);
    auto* dataBuf = new DataBuffer(
        dataCopy,
        static_cast<uint32>(frameSize),
        nullptr,
        true
    );

    simpleBlock.SetParent(*currentCluster_);

    const KaxTrackEntry* trackEntry = (trackNum == VIDEO_TRACK_NUM)
        ? videoTrackEntry_ : audioTrackEntry_;

    simpleBlock.AddFrame(
        *trackEntry,
        timestampMs * 1000000ULL,
        *dataBuf,
        LACING_NONE
    );
    simpleBlock.SetKeyframe(keyframe);
    simpleBlock.SetDiscardable(!keyframe);
}

void MkvPipeline::flushCurrentCluster() {
    if (currentCluster_) {
        lastClusterTimestamp_ = mkvFile_->getFilePointer() - segmentDataStart_;
        currentCluster_->Render(*mkvFile_, *cues_, false);
        delete currentCluster_;
        currentCluster_ = nullptr;
    }
}

void MkvPipeline::finalizeMkvFile() {
    if (!mkvFile_) return;

    flushCurrentCluster();
    writeCues();
    writeSeekHead();

    // Patch Duration in SegmentInfo
    if (durationPlaceholderPos_ > 0 && maxTimestampMs_ > 0) {
        double durationMs = static_cast<double>(maxTimestampMs_);
        uint64_t bits;
        std::memcpy(&bits, &durationMs, 8);
        uint8_t durBytes[8];
        for (int i = 7; i >= 0; --i) {
            durBytes[7 - i] = static_cast<uint8_t>((bits >> (i * 8)) & 0xFF);
        }
        uint64_t savedPos = mkvFile_->getFilePointer();
        mkvFile_->setFilePointer(durationPlaceholderPos_);
        mkvFile_->write(durBytes, 8);
        mkvFile_->setFilePointer(savedPos);
    }

    uint64_t endPos = mkvFile_->getFilePointer();
    if (segmentSizeOffset_ > 0) {
        uint64_t segSize = endPos - segmentDataStart_;

        mkvFile_->setFilePointer(segmentSizeOffset_);

        uint8_t sizeBytes[8];
        sizeBytes[0] = 0x01;
        sizeBytes[1] = static_cast<uint8_t>((segSize >> 48) & 0xFF);
        sizeBytes[2] = static_cast<uint8_t>((segSize >> 40) & 0xFF);
        sizeBytes[3] = static_cast<uint8_t>((segSize >> 32) & 0xFF);
        sizeBytes[4] = static_cast<uint8_t>((segSize >> 24) & 0xFF);
        sizeBytes[5] = static_cast<uint8_t>((segSize >> 16) & 0xFF);
        sizeBytes[6] = static_cast<uint8_t>((segSize >> 8) & 0xFF);
        sizeBytes[7] = static_cast<uint8_t>(segSize & 0xFF);
        mkvFile_->write(sizeBytes, 8);

        mkvFile_->setFilePointer(endPos);
    }

    mkvFile_->close();
    mkvFile_.reset();
    segment_.reset();
    cues_.reset();
}

void MkvPipeline::writeCues() {
    if (cueEntries_.empty()) return;

    std::vector<uint8_t> cuesBody;

    auto appendId = [](std::vector<uint8_t>& buf, uint32_t id, int len) {
        for (int i = len - 1; i >= 0; --i)
            buf.push_back(static_cast<uint8_t>((id >> (i * 8)) & 0xFF));
    };
    auto appendVint = [](std::vector<uint8_t>& buf, uint64_t val, int len) {
        uint8_t marker = static_cast<uint8_t>(1 << (8 - len));
        buf.push_back(marker | static_cast<uint8_t>((val >> ((len - 1) * 8)) & (marker - 1)));
        for (int i = len - 2; i >= 0; --i)
            buf.push_back(static_cast<uint8_t>((val >> (i * 8)) & 0xFF));
    };
    auto appendUint = [&](std::vector<uint8_t>& buf, uint32_t id, int idLen, uint64_t val) {
        appendId(buf, id, idLen);
        int valLen = 1;
        uint64_t tmp = val >> 8;
        while (tmp > 0) { ++valLen; tmp >>= 8; }
        appendVint(buf, valLen, 1);
        for (int i = valLen - 1; i >= 0; --i)
            buf.push_back(static_cast<uint8_t>((val >> (i * 8)) & 0xFF));
    };
    auto appendElement = [&](std::vector<uint8_t>& buf, uint32_t id, int idLen,
                              const std::vector<uint8_t>& data) {
        appendId(buf, id, idLen);
        size_t sz = data.size();
        if (sz < 128) appendVint(buf, sz, 1);
        else appendVint(buf, sz, 2);
        buf.insert(buf.end(), data.begin(), data.end());
    };

    for (auto& entry : cueEntries_) {
        std::vector<uint8_t> trackPos;
        appendUint(trackPos, 0xF7, 1, entry.trackNum);
        appendUint(trackPos, 0xF1, 1, entry.clusterPosition);

        std::vector<uint8_t> cuePoint;
        appendUint(cuePoint, 0xB3, 1, entry.timestampMs);
        appendElement(cuePoint, 0xB7, 1, trackPos);

        appendElement(cuesBody, 0xBB, 1, cuePoint);
    }

    uint8_t cuesId[] = {0x1C, 0x53, 0xBB, 0x6B};
    mkvFile_->write(cuesId, 4);

    uint64_t cuesSize = cuesBody.size();
    uint8_t sizeVint[4] = {
        static_cast<uint8_t>(0x10 | ((cuesSize >> 24) & 0x0F)),
        static_cast<uint8_t>((cuesSize >> 16) & 0xFF),
        static_cast<uint8_t>((cuesSize >> 8) & 0xFF),
        static_cast<uint8_t>(cuesSize & 0xFF)
    };
    mkvFile_->write(sizeVint, 4);
    mkvFile_->write(cuesBody.data(), cuesBody.size());
}

void MkvPipeline::writeSeekHead() {
    // SeekHead is optional for basic playback.
    // The placeholder space remains as EbmlVoid.
}

void MkvPipeline::patchVideoCodecPrivate() {
    if (!spsPpsExtracted_ || codecPrivatePlaceholderPos_ == 0) return;

    std::vector<uint8_t> avcConfig = buildAvcDecoderConfigRecord();
    if (avcConfig.empty()) return;

    std::vector<uint8_t> elem;
    elem.push_back(0x63);
    elem.push_back(0xA2);

    if (avcConfig.size() < 128) {
        elem.push_back(static_cast<uint8_t>(0x80 | avcConfig.size()));
    } else {
        elem.push_back(static_cast<uint8_t>(0x40 | ((avcConfig.size() >> 8) & 0x3F)));
        elem.push_back(static_cast<uint8_t>(avcConfig.size() & 0xFF));
    }
    elem.insert(elem.end(), avcConfig.begin(), avcConfig.end());

    if (elem.size() > codecPrivatePlaceholderSize_) {
        return;
    }

    uint64_t savedPos = mkvFile_->getFilePointer();

    mkvFile_->setFilePointer(codecPrivatePlaceholderPos_);
    mkvFile_->write(elem.data(), elem.size());

    size_t remaining = codecPrivatePlaceholderSize_ - elem.size();
    if (remaining >= 2) {
        uint8_t voidId = 0xEC;
        mkvFile_->write(&voidId, 1);
        size_t voidDataSize = remaining - 2;
        if (voidDataSize < 128) {
            uint8_t vs = static_cast<uint8_t>(0x80 | voidDataSize);
            mkvFile_->write(&vs, 1);
        } else {
            uint8_t vs[2] = {
                static_cast<uint8_t>(0x40 | ((voidDataSize >> 8) & 0x3F)),
                static_cast<uint8_t>(voidDataSize & 0xFF)
            };
            mkvFile_->write(vs, 2);
            voidDataSize = remaining - 3;
        }
        std::vector<uint8_t> zeros(voidDataSize, 0);
        mkvFile_->write(zeros.data(), zeros.size());
    }

    mkvFile_->setFilePointer(savedPos);
}

// ============================================================================
// H.264 Bitstream Helpers
// ============================================================================

bool MkvPipeline::extractSpsPps(const uint8_t* data, size_t size) {
    if (size < 5) return false;

    spsData_.clear();
    ppsData_.clear();

    size_t i = 0;
    std::vector<size_t> startCodePositions;

    while (i + 3 < size) {
        if (i + 3 < size &&
            data[i] == 0x00 && data[i + 1] == 0x00 &&
            data[i + 2] == 0x00 && data[i + 3] == 0x01) {
            startCodePositions.push_back(i + 4);
            i += 4;
        } else if (i + 2 < size &&
                   data[i] == 0x00 && data[i + 1] == 0x00 &&
                   data[i + 2] == 0x01) {
            startCodePositions.push_back(i + 3);
            i += 3;
        } else {
            ++i;
        }
    }

    for (size_t idx = 0; idx < startCodePositions.size(); ++idx) {
        size_t nalStart = startCodePositions[idx];

        size_t nalEnd = size;
        for (size_t j = nalStart + 1; j + 3 <= size; ++j) {
            if ((j + 3 < size &&
                 data[j] == 0 && data[j+1] == 0 &&
                 data[j+2] == 0 && data[j+3] == 1) ||
                (j + 2 < size &&
                 data[j] == 0 && data[j+1] == 0 && data[j+2] == 1)) {
                nalEnd = j;
                break;
            }
        }

        if (nalStart >= nalEnd) continue;

        uint8_t nalType = data[nalStart] & 0x1F;

        if (nalType == 7) {
            spsData_.assign(data + nalStart, data + nalEnd);
        } else if (nalType == 8) {
            ppsData_.assign(data + nalStart, data + nalEnd);
        }
    }

    spsPpsExtracted_ = !spsData_.empty() && !ppsData_.empty();
    return spsPpsExtracted_;
}

std::vector<uint8_t> MkvPipeline::buildAvcDecoderConfigRecord() const {
    if (spsData_.empty() || ppsData_.empty()) {
        return {};
    }

    std::vector<uint8_t> record;
    record.reserve(32 + spsData_.size() + ppsData_.size());

    record.push_back(1);
    record.push_back(spsData_[1]);
    record.push_back(spsData_[2]);
    record.push_back(spsData_[3]);
    record.push_back(0xFF);

    record.push_back(0xE1);
    uint16_t spsLen = static_cast<uint16_t>(spsData_.size());
    record.push_back(static_cast<uint8_t>(spsLen >> 8));
    record.push_back(static_cast<uint8_t>(spsLen & 0xFF));
    record.insert(record.end(), spsData_.begin(), spsData_.end());

    record.push_back(1);
    uint16_t ppsLen = static_cast<uint16_t>(ppsData_.size());
    record.push_back(static_cast<uint8_t>(ppsLen >> 8));
    record.push_back(static_cast<uint8_t>(ppsLen & 0xFF));
    record.insert(record.end(), ppsData_.begin(), ppsData_.end());

    return record;
}

std::vector<uint8_t> MkvPipeline::annexBToAvcc(const uint8_t* data, size_t size) const {
    std::vector<uint8_t> result;
    result.reserve(size);

    std::vector<std::pair<size_t, size_t>> nalUnits;
    size_t i = 0;
    while (i < size) {
        size_t scLen = 0;
        if (i + 3 < size && data[i] == 0 && data[i+1] == 0 &&
            data[i+2] == 0 && data[i+3] == 1) {
            scLen = 4;
        } else if (i + 2 < size && data[i] == 0 && data[i+1] == 0 &&
                   data[i+2] == 1) {
            scLen = 3;
        }

        if (scLen > 0) {
            size_t nalStart = i + scLen;
            size_t nalEnd = size;
            for (size_t j = nalStart + 1; j + 2 < size; ++j) {
                if ((data[j] == 0 && data[j+1] == 0 && data[j+2] == 1) ||
                    (j + 3 < size && data[j] == 0 && data[j+1] == 0 &&
                     data[j+2] == 0 && data[j+3] == 1)) {
                    nalEnd = j;
                    break;
                }
            }
            if (nalStart < nalEnd) {
                nalUnits.emplace_back(nalStart, nalEnd - nalStart);
            }
            i = nalEnd;
        } else {
            ++i;
        }
    }

    for (auto& [start, len] : nalUnits) {
        uint8_t nalType = data[start] & 0x1F;
        if (nalType == 7 || nalType == 8) continue;

        uint32_t nalLen = static_cast<uint32_t>(len);
        result.push_back(static_cast<uint8_t>((nalLen >> 24) & 0xFF));
        result.push_back(static_cast<uint8_t>((nalLen >> 16) & 0xFF));
        result.push_back(static_cast<uint8_t>((nalLen >> 8) & 0xFF));
        result.push_back(static_cast<uint8_t>(nalLen & 0xFF));
        result.insert(result.end(), data + start, data + start + len);
    }

    return result;
}

// ============================================================================
// AAC Helpers
// ============================================================================

std::vector<uint8_t> MkvPipeline::buildAudioSpecificConfig() const {
    uint8_t objectType = 2; // AAC-LC

    uint8_t freqIndex = 0x0F;
    static const int freqTable[] = {
        96000, 88200, 64000, 48000, 44100, 32000,
        24000, 22050, 16000, 12000, 11025, 8000, 7350
    };
    for (int i = 0; i < 13; ++i) {
        if (config_.audio.sampleRate == freqTable[i]) {
            freqIndex = static_cast<uint8_t>(i);
            break;
        }
    }

    uint8_t channelConfig = static_cast<uint8_t>(config_.audio.channelCount);

    std::vector<uint8_t> asc;

    if (freqIndex == 0x0F) {
        uint64_t bits = 0;
        bits = (static_cast<uint64_t>(objectType) << 32) |
               (static_cast<uint64_t>(0x0F) << 28) |
               (static_cast<uint64_t>(config_.audio.sampleRate) << 4) |
               channelConfig;
        asc.resize(5);
        asc[0] = static_cast<uint8_t>((bits >> 32) & 0xFF);
        asc[1] = static_cast<uint8_t>((bits >> 24) & 0xFF);
        asc[2] = static_cast<uint8_t>((bits >> 16) & 0xFF);
        asc[3] = static_cast<uint8_t>((bits >> 8) & 0xFF);
        asc[4] = static_cast<uint8_t>(bits & 0xFF);
    } else {
        uint16_t val = static_cast<uint16_t>(
            (objectType << 11) | (freqIndex << 7) | (channelConfig << 3));
        asc.push_back(static_cast<uint8_t>(val >> 8));
        asc.push_back(static_cast<uint8_t>(val & 0xFF));
    }

    return asc;
}

// ============================================================================
// Opus Helpers
// ============================================================================

std::vector<uint8_t> MkvPipeline::buildOpusHead() const {
    std::vector<uint8_t> head;
    head.reserve(19);

    const char magic[] = "OpusHead";
    head.insert(head.end(), magic, magic + 8);

    head.push_back(1);
    head.push_back(static_cast<uint8_t>(config_.audio.channelCount));

    uint16_t preSkip = 3840;
    head.push_back(static_cast<uint8_t>(preSkip & 0xFF));
    head.push_back(static_cast<uint8_t>(preSkip >> 8));

    uint32_t sr = config_.audio.sampleRate;
    head.push_back(static_cast<uint8_t>(sr & 0xFF));
    head.push_back(static_cast<uint8_t>((sr >> 8) & 0xFF));
    head.push_back(static_cast<uint8_t>((sr >> 16) & 0xFF));
    head.push_back(static_cast<uint8_t>((sr >> 24) & 0xFF));

    head.push_back(0);
    head.push_back(0);

    head.push_back(0);

    return head;
}

// ============================================================================
// Vorbis Helpers
// ============================================================================

std::vector<uint8_t> MkvPipeline::buildVorbisCodecPrivate() const {
    if (vorbisHeaders_.size() != 3) return {};

    std::vector<uint8_t> result;

    result.push_back(2);

    size_t size0 = vorbisHeaders_[0].size();
    while (size0 >= 255) {
        result.push_back(255);
        size0 -= 255;
    }
    result.push_back(static_cast<uint8_t>(size0));

    size_t size1 = vorbisHeaders_[1].size();
    while (size1 >= 255) {
        result.push_back(255);
        size1 -= 255;
    }
    result.push_back(static_cast<uint8_t>(size1));

    for (const auto& hdr : vorbisHeaders_) {
        result.insert(result.end(), hdr.begin(), hdr.end());
    }

    return result;
}

// ============================================================================
// Utility
// ============================================================================

void MkvPipeline::reportError(const std::string& msg) {
    if (errorCallback_) {
        errorCallback_(msg);
    }
}

void MkvPipeline::releaseResources() {
    sinkWriter_.reset();
    shutdownAudioEncoder();

    if (mkvFile_) {
        mkvFile_->close();
        mkvFile_.reset();
    }

    if (currentCluster_) {
        delete currentCluster_;
        currentCluster_ = nullptr;
    }

    segment_.reset();
    cues_.reset();
    cueEntries_.clear();
    bufferedAudioPackets_.clear();

    d3dDevice_.Reset();

    initialized_ = false;
}

int64_t MkvPipeline::toMkvTimestamp(int64_t hns) const {
    return hns / 10000;
}

} // namespace pb
