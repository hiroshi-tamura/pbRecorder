#include "RecordingSession.h"
#include "core/D3DManager.h"
#include "capture/DxgiScreenCapture.h"
#include "capture/WindowCapture.h"
#include "capture/RegionCapture.h"
#include "audio/WasapiCapture.h"
#include "audio/AsioCapture.h"
#include "pipeline/SinkWriterPipeline.h"
#include "pipeline/MkvPipeline.h"

namespace pb {

RecordingSession::RecordingSession() = default;

RecordingSession::~RecordingSession() {
    if (recording_.load()) {
        stop();
    }
}

bool RecordingSession::initialize(const RecordingConfig& config) {
    if (recording_.load()) {
        onError("Cannot initialize while recording");
        return false;
    }

    config_ = config;

    // Ensure D3D device
    try {
        D3DManager::instance().initialize();
        device_ = D3DManager::instance().getDevice();
    } catch (const std::exception& e) {
        onError(std::string("D3D initialization failed: ") + e.what());
        return false;
    }

    // Create capture source
    captureSource_ = createCaptureSource(config_.capture.mode);
    if (!captureSource_) {
        onError("Failed to create capture source");
        return false;
    }

    if (!captureSource_->initialize(config_.capture, device_)) {
        onError("Failed to initialize capture source");
        return false;
    }

    // Auto-detect video dimensions from capture source
    if (config_.video.width == 0) config_.video.width = captureSource_->getWidth();
    if (config_.video.height == 0) config_.video.height = captureSource_->getHeight();

    // Create audio sources
    if (config_.recordAudio) {
        // Output audio (system/loopback)
        if (config_.useOutputAudio && !config_.outputAudioDevice.id.empty()) {
            outputAudioSource_ = createAudioSource(config_.outputAudioDevice.type);
            if (outputAudioSource_) {
                if (!outputAudioSource_->initialize(config_.outputAudioDevice)) {
                    onError("出力オーディオデバイスの初期化に失敗しました");
                    outputAudioSource_.reset();
                }
            }
        }

        // Input audio (microphone)
        if (config_.useInputAudio && !config_.inputAudioDevice.id.empty()) {
            inputAudioSource_ = createAudioSource(config_.inputAudioDevice.type);
            if (inputAudioSource_) {
                if (!inputAudioSource_->initialize(config_.inputAudioDevice)) {
                    onError("入力オーディオデバイスの初期化に失敗しました");
                    inputAudioSource_.reset();
                }
            }
        }

        // If neither source was created, disable audio recording
        if (!outputAudioSource_ && !inputAudioSource_) {
            config_.recordAudio = false;
        } else {
            // Use actual audio source parameters for pipeline configuration
            // to ensure format matches between capture and encoder
            IAudioSource* primaryAudio = outputAudioSource_ ? outputAudioSource_.get()
                                                             : inputAudioSource_.get();
            config_.audio.sampleRate = primaryAudio->getSampleRate();
            config_.audio.channelCount = primaryAudio->getChannelCount();
            config_.audio.bitsPerSample = primaryAudio->getBitsPerSample();
        }
    }

    // Create pipeline
    pipeline_ = createPipeline(config_.container);
    if (!pipeline_) {
        onError("Failed to create recording pipeline");
        return false;
    }
    pipeline_->setErrorCallback([this](const std::string& e) { onError(e); });

    if (!pipeline_->initialize(config_, device_)) {
        onError("Failed to initialize recording pipeline");
        return false;
    }

    initialized_.store(true);
    return true;
}

bool RecordingSession::start() {
    if (!initialized_.load()) {
        onError("Session not initialized");
        return false;
    }
    if (recording_.load()) {
        onError("Already recording");
        return false;
    }

    // Clear queues
    videoQueue_.clear();
    audioQueue_.clear();
    pauseOffset_ = 0;
    pauseStartTime_ = 0;

    // Start pipeline
    if (!pipeline_->start()) {
        onError("Failed to start pipeline");
        return false;
    }

    recording_.store(true);
    paused_.store(false);

    // Start writer threads
    videoWriterThread_ = std::thread(&RecordingSession::videoWriterThread, this);
    audioWriterThread_ = std::thread(&RecordingSession::audioWriterThread, this);

    // Set callbacks and start capture
    captureSource_->setFrameCallback([this](const VideoFrame& f) { onVideoFrame(f); });
    captureSource_->setErrorCallback([this](const std::string& e) { onError(e); });

    if (!captureSource_->start()) {
        onError("Failed to start capture");
        stop();
        return false;
    }

    // Start audio capture
    if (config_.recordAudio) {
        if (outputAudioSource_) {
            outputAudioSource_->setAudioCallback([this](const AudioBuffer& b) { onAudioBuffer(b); });
            outputAudioSource_->setErrorCallback([this](const std::string& e) { onError(e); });
            if (!outputAudioSource_->start()) {
                onError("出力オーディオの開始に失敗しました");
            }
        }
        if (inputAudioSource_) {
            inputAudioSource_->setAudioCallback([this](const AudioBuffer& b) { onAudioBuffer(b); });
            inputAudioSource_->setErrorCallback([this](const std::string& e) { onError(e); });
            if (!inputAudioSource_->start()) {
                onError("入力オーディオの開始に失敗しました");
            }
        }
    }

    return true;
}

bool RecordingSession::stop() {
    if (!recording_.load() && !initialized_.load()) {
        return true;
    }

    recording_.store(false);
    paused_.store(false);

    // Stop capture sources first
    if (captureSource_) {
        captureSource_->stop();
    }
    if (outputAudioSource_) {
        outputAudioSource_->stop();
    }
    if (inputAudioSource_) {
        inputAudioSource_->stop();
    }
    if (audioSource_) {
        audioSource_->stop();
    }

    // Stop queues to unblock writer threads
    videoQueue_.stop();
    audioQueue_.stop();

    // Wait for writer threads
    if (videoWriterThread_.joinable()) {
        videoWriterThread_.join();
    }
    if (audioWriterThread_.joinable()) {
        audioWriterThread_.join();
    }

    // Finalize pipeline
    if (pipeline_) {
        pipeline_->stop();
    }

    // Release resources
    captureSource_.reset();
    audioSource_.reset();
    outputAudioSource_.reset();
    inputAudioSource_.reset();
    pipeline_.reset();
    initialized_.store(false);

    return true;
}

bool RecordingSession::pause() {
    if (!recording_.load() || paused_.load()) return false;

    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        pauseStartTime_ = (now.QuadPart * 10000000LL) / freq.QuadPart;
    }

    paused_.store(true);
    return true;
}

bool RecordingSession::resume() {
    if (!recording_.load() || !paused_.load()) return false;

    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        int64_t resumeTime = (now.QuadPart * 10000000LL) / freq.QuadPart;
        pauseOffset_ += (resumeTime - pauseStartTime_);
        pauseStartTime_ = 0;
    }

    paused_.store(false);
    return true;
}

int64_t RecordingSession::getDurationMs() const {
    if (pipeline_) return pipeline_->getDurationMs();
    return 0;
}

int64_t RecordingSession::getFileSize() const {
    if (pipeline_) return pipeline_->getFileSize();
    return 0;
}

void RecordingSession::onVideoFrame(const VideoFrame& frame) {
    if (!recording_.load() || paused_.load()) return;

    // Adjust timestamp for pause offset
    VideoFrame adjusted = frame;
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        adjusted.timestamp -= pauseOffset_;
    }

    videoQueue_.push(std::move(adjusted));
}

void RecordingSession::onAudioBuffer(const AudioBuffer& buffer) {
    if (!recording_.load() || paused_.load()) return;

    AudioBuffer adjusted = buffer;
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        adjusted.timestamp -= pauseOffset_;
    }

    audioQueue_.push(std::move(adjusted));
}

void RecordingSession::onError(const std::string& error) {
    if (errorCallback_) {
        errorCallback_(error);
    }
}

void RecordingSession::videoWriterThread() {
    while (recording_.load()) {
        VideoFrame frame;
        if (videoQueue_.tryPop(frame, std::chrono::milliseconds(50))) {
            if (!pipeline_->writeVideoFrame(frame)) {
                onError("Failed to write video frame");
                break;
            }
        }
    }

    // Drain remaining frames
    VideoFrame frame;
    while (videoQueue_.tryPop(frame, std::chrono::milliseconds(1))) {
        pipeline_->writeVideoFrame(frame);
    }
}

void RecordingSession::audioWriterThread() {
    if (!config_.recordAudio) return;

    while (recording_.load()) {
        AudioBuffer buffer;
        if (audioQueue_.tryPop(buffer, std::chrono::milliseconds(50))) {
            if (!pipeline_->writeAudioSamples(buffer)) {
                onError("Failed to write audio samples");
                break;
            }
        }
    }

    // Drain remaining buffers
    AudioBuffer buffer;
    while (audioQueue_.tryPop(buffer, std::chrono::milliseconds(1))) {
        pipeline_->writeAudioSamples(buffer);
    }
}

std::unique_ptr<ICaptureSource> RecordingSession::createCaptureSource(CaptureMode mode) {
    switch (mode) {
        case CaptureMode::Screen:
            return std::make_unique<DxgiScreenCapture>();
        case CaptureMode::Window:
            return std::make_unique<WindowCapture>();
        case CaptureMode::Region:
            return std::make_unique<RegionCapture>();
        default:
            return nullptr;
    }
}

std::unique_ptr<IAudioSource> RecordingSession::createAudioSource(AudioDeviceType type) {
    switch (type) {
        case AudioDeviceType::WASAPI_Render:
        case AudioDeviceType::WASAPI_Capture:
            return std::make_unique<WasapiCapture>();
#ifdef ASIO_AVAILABLE
        case AudioDeviceType::ASIO:
            return std::make_unique<AsioCapture>();
#endif
        default:
            return std::make_unique<WasapiCapture>();
    }
}

std::unique_ptr<IRecordingPipeline> RecordingSession::createPipeline(ContainerFormat format) {
    switch (format) {
        case ContainerFormat::MP4:
        case ContainerFormat::WMV:
            return std::make_unique<SinkWriterPipeline>();
        case ContainerFormat::MKV:
            return std::make_unique<MkvPipeline>();
        default:
            return nullptr;
    }
}

} // namespace pb
