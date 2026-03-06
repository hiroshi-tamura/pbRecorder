#pragma once

#include "pipeline/IRecordingPipeline.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <matroska/KaxSegment.h>
#include <matroska/KaxTracks.h>
#include <matroska/KaxTrackEntryData.h>
#include <matroska/KaxTrackAudio.h>
#include <matroska/KaxTrackVideo.h>
#include <matroska/KaxCluster.h>
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

#include <mutex>
#include <atomic>
#include <vector>
#include <string>
#include <memory>

// Forward declarations for optional codecs
struct OpusEncoder;
struct vorbis_info;
struct vorbis_dsp_state;
struct vorbis_block;
struct vorbis_comment;

namespace pb {

class SinkWriterPipeline;

class MkvPipeline : public IRecordingPipeline {
public:
    MkvPipeline();
    ~MkvPipeline() override;

    bool initialize(const RecordingConfig& config, ID3D11Device* device) override;
    bool start() override;
    bool stop() override;
    bool writeVideoFrame(const VideoFrame& frame) override;
    bool writeAudioSamples(const AudioBuffer& buffer) override;
    void setErrorCallback(ErrorCallback callback) override;
    bool isRecording() const override;
    int64_t getDurationMs() const override;
    int64_t getFileSize() const override;

private:
    // ---- Audio encoder (Opus/Vorbis — AAC goes through SinkWriter) ----
    bool initializeAudioEncoder();
    bool initializeOpusEncoder();
    bool initializeVorbisEncoder();
    bool encodeAudioOpus(const AudioBuffer& buffer,
                         std::vector<std::vector<uint8_t>>& packets);
    bool encodeAudioVorbis(const AudioBuffer& buffer,
                           std::vector<std::vector<uint8_t>>& packets);
    void shutdownAudioEncoder();

    // ---- Remux: temp .mp4 → .mkv ----
    bool remuxToMkv();

    // ---- MKV muxing (libmatroska) ----
    bool initializeMkvWriter();
    bool writeEbmlHeader();
    bool writeSegmentInfo();
    bool writeTracks();
    void writeClusterData(uint8_t trackNum, int64_t timestampMs,
                          const uint8_t* data, size_t size, bool keyframe);
    void flushCurrentCluster();
    void finalizeMkvFile();
    void writeCues();
    void writeSeekHead();

    // ---- H.264 bitstream helpers ----
    bool extractSpsPps(const uint8_t* data, size_t size);
    std::vector<uint8_t> buildAvcDecoderConfigRecord() const;
    std::vector<uint8_t> annexBToAvcc(const uint8_t* data, size_t size) const;
    void patchVideoCodecPrivate();

    // ---- Audio codec private helpers ----
    std::vector<uint8_t> buildAudioSpecificConfig() const;
    std::vector<uint8_t> buildOpusHead() const;
    std::vector<uint8_t> buildVorbisCodecPrivate() const;

    // ---- Utility ----
    void reportError(const std::string& msg);
    void releaseResources();
    int64_t toMkvTimestamp(int64_t hns) const;

    // ---- Config ----
    RecordingConfig config_;

    // ---- Internal SinkWriter for H.264 encoding ----
    std::unique_ptr<SinkWriterPipeline> sinkWriter_;
    ComPtr<ID3D11Device> d3dDevice_;
    std::wstring tempMp4Path_;
    bool sinkWriterHasAudio_ = false; // true when AAC audio goes through SinkWriter

    // ---- Audio buffer (for Opus/Vorbis/PCM — not through SinkWriter) ----
    struct AudioPacket {
        std::vector<uint8_t> data;
        int64_t timestampMs;
    };
    std::vector<AudioPacket> bufferedAudioPackets_;
    std::mutex audioBufferMutex_;

    // ---- Opus encoder ----
    OpusEncoder* opusEncoder_ = nullptr;
    int opusFrameSamples_ = 0;
    std::vector<float> opusResidualBuffer_;

    // ---- Vorbis encoder ----
    vorbis_info* vorbisInfo_ = nullptr;
    vorbis_dsp_state* vorbisDsp_ = nullptr;
    vorbis_block* vorbisBlock_ = nullptr;
    vorbis_comment* vorbisComment_ = nullptr;
    std::vector<std::vector<uint8_t>> vorbisHeaders_;

    // ---- MKV writer (used during remux in stop()) ----
    std::unique_ptr<libebml::StdIOCallback> mkvFile_;
    std::unique_ptr<libmatroska::KaxSegment> segment_;
    std::unique_ptr<libmatroska::KaxCues> cues_;
    libmatroska::KaxCluster* currentCluster_ = nullptr;
    uint64_t segmentDataStart_ = 0;
    uint64_t segmentSizeOffset_ = 0;
    uint64_t codecPrivatePlaceholderPos_ = 0;
    uint64_t codecPrivatePlaceholderSize_ = 0;
    int64_t clusterStartTimestamp_ = -1;
    int64_t lastClusterTimestamp_ = 0;
    static constexpr int64_t CLUSTER_DURATION_MS = 1000;

    static constexpr uint8_t VIDEO_TRACK_NUM = 1;
    static constexpr uint8_t AUDIO_TRACK_NUM = 2;

    libmatroska::KaxTrackEntry* videoTrackEntry_ = nullptr;
    libmatroska::KaxTrackEntry* audioTrackEntry_ = nullptr;

    struct CueEntry {
        int64_t timestampMs;
        uint8_t trackNum;
        uint64_t clusterPosition;
    };
    std::vector<CueEntry> cueEntries_;

    uint64_t seekHeadPlaceholderPos_ = 0;
    uint64_t seekHeadPlaceholderSize_ = 0;
    int64_t maxTimestampMs_ = 0;
    uint64_t segmentInfoPos_ = 0;
    uint64_t durationPlaceholderPos_ = 0;

    // ---- H.264 data ----
    bool spsPpsExtracted_ = false;
    std::vector<uint8_t> spsData_;
    std::vector<uint8_t> ppsData_;

    // ---- State ----
    std::atomic<bool> recording_{false};
    std::atomic<bool> initialized_{false};
    bool hasAudio_ = false;

    // ---- Timing ----
    std::atomic<int64_t> firstAudioTimestamp_{-1};

    // ---- Thread safety ----
    std::mutex encodeMutex_;

    ErrorCallback errorCallback_;
};

} // namespace pb
