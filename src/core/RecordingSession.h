#pragma once

#include "core/Types.h"
#include "capture/ICaptureSource.h"
#include "audio/IAudioSource.h"
#include "pipeline/IRecordingPipeline.h"

#include <thread>
#include <atomic>
#include <memory>
#include <string>

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

private:
    void onVideoFrame(const VideoFrame& frame);
    void onAudioBuffer(const AudioBuffer& buffer);
    void onError(const std::string& error);

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

    int64_t pauseOffset_ = 0;
    int64_t pauseStartTime_ = 0;
    std::mutex pauseMutex_;

    ErrorCallback errorCallback_;
    ID3D11Device* device_ = nullptr;
};

} // namespace pb
