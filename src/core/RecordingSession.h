#pragma once

#include "core/Types.h"
#include "capture/ICaptureSource.h"
#include "audio/IAudioSource.h"
#include "pipeline/IRecordingPipeline.h"

#include <thread>
#include <atomic>
#include <memory>
#include <functional>
#include <string>
#include <mutex>

namespace pb {

class RecordingSession {
public:
    RecordingSession();
    ~RecordingSession();

    // Non-copyable
    RecordingSession(const RecordingSession&) = delete;
    RecordingSession& operator=(const RecordingSession&) = delete;

    bool initialize(const RecordingConfig& config);
    bool start();
    bool stop();
    bool pause();
    bool resume();

    bool isRecording() const { return recording_.load(); }
    bool isPaused() const { return paused_.load(); }
    int64_t getDurationMs() const;
    int64_t getFileSize() const;

    void setErrorCallback(ErrorCallback callback) { errorCallback_ = std::move(callback); }
    void setAudioLevelCallback(std::function<void(float)> callback) { audioLevelCallback_ = std::move(callback); }

private:
    void onVideoFrame(const VideoFrame& frame);
    void onAudioBuffer(const AudioBuffer& buffer);
    void onError(const std::string& error);
    int64_t normalizeTimestamp(int64_t timestamp100ns) const;
    int64_t normalizeMediaTimestamp(int64_t timestamp100ns) const;
    void ensureMediaOriginLocked(int64_t timestamp100ns);
    void enqueueVideoFrameLocked(VideoFrame frame);
    void enqueueAudioBufferLocked(AudioBuffer buffer);
    bool writeProcessedAudioBuffer(AudioBuffer buffer);
    bool writeLeadingSilenceIfNeeded(const AudioBuffer& firstBuffer);

    void videoWriterThread();
    void audioWriterThread();

    std::unique_ptr<ICaptureSource> createCaptureSource(CaptureMode mode);
    std::unique_ptr<IAudioSource> createAudioSource(AudioDeviceType type);
    std::unique_ptr<IRecordingPipeline> createPipeline(ContainerFormat format);

    RecordingConfig config_;
    std::unique_ptr<ICaptureSource> captureSource_;
    std::unique_ptr<IAudioSource> audioSource_;
    std::unique_ptr<IAudioSource> outputAudioSource_; // system audio (loopback)
    std::unique_ptr<IAudioSource> inputAudioSource_;  // microphone
    std::unique_ptr<IRecordingPipeline> pipeline_;

    ThreadSafeQueue<VideoFrame> videoQueue_;
    ThreadSafeQueue<AudioBuffer> audioQueue_;

    std::thread videoWriterThread_;
    std::thread audioWriterThread_;

    std::atomic<bool> recording_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> writerFailed_{false};
    std::mutex stopMutex_;

    int64_t pauseOffset_ = 0;
    int64_t pauseStartTime_ = 0;
    int64_t recordingStartTimestamp_ = 0;
    int64_t mediaOriginTimestamp_ = -1;
    int64_t firstVideoTimestamp_ = -1;
    int64_t firstAudioTimestamp_ = -1;
    std::mutex pauseMutex_;

    ErrorCallback errorCallback_;
    std::function<void(float)> audioLevelCallback_;
    ID3D11Device* device_ = nullptr;

    // Audio resampling (when ASIO sample rate differs from pipeline target)
    AudioBuffer resampleBuffer(const AudioBuffer& input, uint32_t targetRate);
    AudioBuffer normalizeAudioChannels(const AudioBuffer& input, uint32_t targetChannels);
    uint32_t sourceSampleRate_ = 0;
    uint32_t sourceChannelCount_ = 0;
    bool needsResample_ = false;
    bool needsChannelMix_ = false;
    bool audioPrimed_ = false;
    int64_t expectedAudioTimestamp_ = -1;
    uint64_t resampleInputFramesTotal_ = 0;
    uint64_t resampleOutputFramesTotal_ = 0;
};

} // namespace pb
