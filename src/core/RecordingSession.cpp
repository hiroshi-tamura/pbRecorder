#include "RecordingSession.h"
#include "core/D3DManager.h"
#include "capture/DxgiScreenCapture.h"
#include "capture/WindowCapture.h"
#include "capture/RegionCapture.h"
#include "capture/UiElementCapture.h"
#include "audio/WasapiCapture.h"
#include "audio/AsioCapture.h"
#include "pipeline/SinkWriterPipeline.h"
#include "pipeline/MkvPipeline.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>

namespace pb {

namespace {

uint32_t nearestSupportedRate(uint32_t requested, const uint32_t* rates, size_t count)
{
    if (count == 0 || requested == 0) {
        return requested;
    }

    uint32_t best = rates[0];
    uint32_t bestDelta = requested > best ? requested - best : best - requested;
    for (size_t i = 1; i < count; ++i) {
        const uint32_t rate = rates[i];
        const uint32_t delta = requested > rate ? requested - rate : rate - requested;
        if (delta < bestDelta) {
            best = rate;
            bestDelta = delta;
        }
    }
    return best;
}

uint32_t supportedSampleRateForCodec(AudioCodec codec, uint32_t requested)
{
    if (requested == 0) {
        requested = 48000;
    }

    static constexpr uint32_t opusRates[] = {8000, 12000, 16000, 24000, 48000};
    static constexpr uint32_t mfCommonRates[] = {22050, 32000, 44100, 48000};

    switch (codec) {
    case AudioCodec::Opus:
        return nearestSupportedRate(requested, opusRates, std::size(opusRates));
    case AudioCodec::AAC:
    case AudioCodec::MP3:
        return nearestSupportedRate(requested, mfCommonRates, std::size(mfCommonRates));
    case AudioCodec::WMA:
        return 48000;
    case AudioCodec::Vorbis:
    case AudioCodec::PCM:
    default:
        return requested;
    }
}

int64_t currentQpcTime100ns()
{
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return static_cast<int64_t>(
        (static_cast<double>(now.QuadPart) / static_cast<double>(freq.QuadPart)) * 10000000.0);
}

} // namespace

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

    // Align to 16-pixel boundary for encoder compatibility (avoids garbled output)
    config_.video.width  = (config_.video.width  + 15) & ~15;
    config_.video.height = (config_.video.height + 15) & ~15;

    // Create audio sources
    if (config_.recordAudio) {
        if (config_.useOutputAudio && config_.useInputAudio) {
            config_.useInputAudio = false;
        }

        // Check if both output and input use the same ASIO driver
        // ASIO SDK is a singleton — only one driver instance allowed at a time
        bool outputIsAsio = config_.useOutputAudio &&
            (config_.outputAudioDevice.type == AudioDeviceType::ASIO ||
             config_.outputAudioDevice.type == AudioDeviceType::ASIO_Output);
        bool inputIsAsio = config_.useInputAudio &&
            (config_.inputAudioDevice.type == AudioDeviceType::ASIO ||
             config_.inputAudioDevice.type == AudioDeviceType::ASIO_Output);

        if (outputIsAsio && inputIsAsio) {
            // Cannot use two ASIO instances simultaneously — prioritize output
            onError("ASIO出力と入力を同時に使用できません。出力のみ使用します。");
            config_.useInputAudio = false;
        }

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
            IAudioSource* primaryAudio = outputAudioSource_ ? outputAudioSource_.get()
                                                             : inputAudioSource_.get();
            sourceSampleRate_ = primaryAudio->getSampleRate();
            sourceChannelCount_ = primaryAudio->getChannelCount();
            config_.audio.channelCount = sourceChannelCount_;
            config_.audio.bitsPerSample = primaryAudio->getBitsPerSample();

            uint32_t maxCodecChannels = 0;
            switch (config_.audio.codec) {
            case AudioCodec::AAC:  maxCodecChannels = 6; break;
            case AudioCodec::MP3:
            case AudioCodec::WMA:
            case AudioCodec::Opus: maxCodecChannels = 2; break;
            case AudioCodec::Vorbis:
            case AudioCodec::PCM:
            default: break;
            }
            if (maxCodecChannels > 0 && config_.audio.channelCount > static_cast<int>(maxCodecChannels)) {
                config_.audio.channelCount = static_cast<int>(maxCodecChannels);
            }
            needsChannelMix_ = sourceChannelCount_ != static_cast<uint32_t>(config_.audio.channelCount);

            const uint32_t requestedRate = static_cast<uint32_t>(config_.audio.sampleRate);
            uint32_t targetRate = supportedSampleRateForCodec(config_.audio.codec, requestedRate);
            config_.audio.sampleRate = targetRate;
            needsResample_ = (sourceSampleRate_ != targetRate);
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
    recordingStartTimestamp_ = currentQpcTime100ns();
    mediaOriginTimestamp_ = recordingStartTimestamp_;
    firstVideoTimestamp_ = -1;
    firstAudioTimestamp_ = -1;
    audioPrimed_ = false;
    expectedAudioTimestamp_ = -1;
    resampleInputFramesTotal_ = 0;
    resampleOutputFramesTotal_ = 0;

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
                stop();
                return false;
            }
        }
        if (inputAudioSource_) {
            inputAudioSource_->setAudioCallback([this](const AudioBuffer& b) { onAudioBuffer(b); });
            inputAudioSource_->setErrorCallback([this](const std::string& e) { onError(e); });
            if (!inputAudioSource_->start()) {
                onError("入力オーディオの開始に失敗しました");
                stop();
                return false;
            }
        }
    }

    return true;
}

bool RecordingSession::stop() {
    std::lock_guard<std::mutex> stopLock(stopMutex_);

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
    bool stopOk = !writerFailed_.load();
    if (pipeline_) {
        stopOk = pipeline_->stop() && stopOk;
        if (!stopOk) {
            onError("Recording finalization failed. The file may be incomplete or not seekable.");
        }
    }

    // Release resources
    captureSource_.reset();
    audioSource_.reset();
    outputAudioSource_.reset();
    inputAudioSource_.reset();
    pipeline_.reset();
    initialized_.store(false);
    writerFailed_.store(false);

    return stopOk;
}

bool RecordingSession::pause() {
    if (!recording_.load() || paused_.load()) return false;

    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        pauseStartTime_ = currentQpcTime100ns();
    }

    paused_.store(true);
    return true;
}

bool RecordingSession::resume() {
    if (!recording_.load() || !paused_.load()) return false;

    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        int64_t resumeTime = currentQpcTime100ns();
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

int64_t RecordingSession::normalizeTimestamp(int64_t timestamp100ns) const {
    int64_t normalized = timestamp100ns - recordingStartTimestamp_;
    if (normalized < 0) {
        normalized = 0;
    }
    return normalized;
}

int64_t RecordingSession::normalizeMediaTimestamp(int64_t timestamp100ns) const {
    int64_t normalized = timestamp100ns - mediaOriginTimestamp_;
    if (normalized < 0) {
        normalized = 0;
    }
    return normalized;
}

void RecordingSession::ensureMediaOriginLocked(int64_t timestamp100ns) {
    if (mediaOriginTimestamp_ >= 0) {
        return;
    }

    mediaOriginTimestamp_ = timestamp100ns;
}

void RecordingSession::enqueueVideoFrameLocked(VideoFrame frame) {
    frame.timestamp = normalizeMediaTimestamp(frame.timestamp);
    frame.timestamp -= pauseOffset_;
    if (frame.timestamp < 0) frame.timestamp = 0;

    const size_t maxQueuedFrames = static_cast<size_t>(
        std::clamp(config_.video.fps * 2, 5, 240));
    videoQueue_.pushBounded(std::move(frame), maxQueuedFrames);
}

void RecordingSession::enqueueAudioBufferLocked(AudioBuffer buffer) {
    buffer.timestamp = normalizeMediaTimestamp(buffer.timestamp);
    buffer.timestamp -= pauseOffset_;
    if (buffer.timestamp < 0) buffer.timestamp = 0;
    audioQueue_.pushBounded(std::move(buffer), 600);
}

void RecordingSession::onVideoFrame(const VideoFrame& frame) {
    if (!recording_.load() || paused_.load()) return;

    VideoFrame adjusted = frame;
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        if (firstVideoTimestamp_ < 0) {
            firstVideoTimestamp_ = adjusted.timestamp;
        }
        enqueueVideoFrameLocked(std::move(adjusted));
    }
}

void RecordingSession::onAudioBuffer(const AudioBuffer& buffer) {
    if (!recording_.load() || paused_.load()) return;

    AudioBuffer adjusted = buffer;
    if (audioLevelCallback_) {
        float peak = 0.0f;
        if (adjusted.bitsPerSample == 16) {
            const auto* samples = reinterpret_cast<const int16_t*>(adjusted.data.data());
            const size_t count = adjusted.data.size() / sizeof(int16_t);
            int maxAbs = 0;
            for (size_t i = 0; i < count; ++i) {
                maxAbs = std::max(maxAbs, std::abs(static_cast<int>(samples[i])));
            }
            peak = std::min(1.0f, static_cast<float>(maxAbs) / 32768.0f);
        } else if (adjusted.bitsPerSample == 32) {
            const auto* samples = reinterpret_cast<const float*>(adjusted.data.data());
            const size_t count = adjusted.data.size() / sizeof(float);
            for (size_t i = 0; i < count; ++i) {
                peak = std::max(peak, std::min(1.0f, std::abs(samples[i])));
            }
        }
        audioLevelCallback_(peak);
    }
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        if (firstAudioTimestamp_ < 0) {
            firstAudioTimestamp_ = adjusted.timestamp;
        }
        enqueueAudioBufferLocked(std::move(adjusted));
    }
}

void RecordingSession::onError(const std::string& error) {
    if (errorCallback_) {
        errorCallback_(error);
    }
}

void RecordingSession::videoWriterThread() {
    try {
        while (recording_.load()) {
            VideoFrame frame;
            if (videoQueue_.tryPop(frame, std::chrono::milliseconds(50))) {
                if (!pipeline_->writeVideoFrame(frame)) {
                    writerFailed_.store(true);
                    onError("Failed to write video frame");
                    break;
                }
            }
        }

        // Drain remaining frames
        VideoFrame frame;
        while (videoQueue_.tryPop(frame, std::chrono::milliseconds(1))) {
            if (!pipeline_->writeVideoFrame(frame)) {
                writerFailed_.store(true);
                onError("Failed to write remaining video frame");
                break;
            }
        }
    } catch (const std::exception& e) {
        writerFailed_.store(true);
        recording_.store(false);
        onError(std::string("Video writer thread failed: ") + e.what());
    } catch (...) {
        writerFailed_.store(true);
        recording_.store(false);
        onError("Video writer thread failed with an unknown error");
    }
}

void RecordingSession::audioWriterThread() {
    if (!config_.recordAudio) return;

    try {
        while (recording_.load()) {
            AudioBuffer buffer;
            if (audioQueue_.tryPop(buffer, std::chrono::milliseconds(50))) {
                if (!writeProcessedAudioBuffer(std::move(buffer))) {
                    writerFailed_.store(true);
                    onError("Failed to write audio samples");
                    break;
                }
            }
        }

        // Drain remaining buffers
        AudioBuffer buffer;
        while (audioQueue_.tryPop(buffer, std::chrono::milliseconds(1))) {
            if (!writeProcessedAudioBuffer(std::move(buffer))) {
                writerFailed_.store(true);
                onError("Failed to write remaining audio samples");
                break;
            }
        }
    } catch (const std::exception& e) {
        writerFailed_.store(true);
        recording_.store(false);
        onError(std::string("Audio writer thread failed: ") + e.what());
    } catch (...) {
        writerFailed_.store(true);
        recording_.store(false);
        onError("Audio writer thread failed with an unknown error");
    }
}

bool RecordingSession::writeProcessedAudioBuffer(AudioBuffer buffer) {
    if (needsResample_) {
        buffer = resampleBuffer(buffer, config_.audio.sampleRate);
    }
    if (needsChannelMix_) {
        buffer = normalizeAudioChannels(buffer, config_.audio.channelCount);
    }
    if (!writeLeadingSilenceIfNeeded(buffer)) {
        return false;
    }
    if (expectedAudioTimestamp_ >= 0 && buffer.timestamp > expectedAudioTimestamp_) {
        const int64_t gap = buffer.timestamp - expectedAudioTimestamp_;
        // Ignore sub-millisecond jitter, but preserve real device/queue gaps.
        if (gap > 10000 && buffer.sampleRate > 0 && buffer.channelCount > 0 && buffer.bitsPerSample > 0) {
            AudioBuffer silence;
            silence.timestamp = expectedAudioTimestamp_;
            silence.sampleRate = buffer.sampleRate;
            silence.channelCount = buffer.channelCount;
            silence.bitsPerSample = buffer.bitsPerSample;
            const uint64_t frames = static_cast<uint64_t>(gap) *
                                    static_cast<uint64_t>(buffer.sampleRate) / 10000000ULL;
            silence.sampleCount = static_cast<uint32_t>(
                std::min<uint64_t>(frames, std::numeric_limits<uint32_t>::max()));
            const uint32_t bytesPerSample = silence.bitsPerSample / 8;
            silence.data.resize(static_cast<size_t>(silence.sampleCount) *
                                silence.channelCount * bytesPerSample, 0);
            if (silence.sampleCount > 0 && !pipeline_->writeAudioSamples(silence)) {
                return false;
            }
        }
    } else if (expectedAudioTimestamp_ >= 0 && buffer.timestamp < expectedAudioTimestamp_) {
        buffer.timestamp = expectedAudioTimestamp_;
    }
    expectedAudioTimestamp_ = buffer.timestamp +
        static_cast<int64_t>(buffer.sampleCount) * 10000000LL /
            static_cast<int64_t>(buffer.sampleRate > 0 ? buffer.sampleRate : 48000);
    return pipeline_->writeAudioSamples(buffer);
}

bool RecordingSession::writeLeadingSilenceIfNeeded(const AudioBuffer& firstBuffer) {
    if (audioPrimed_) {
        return true;
    }
    audioPrimed_ = true;

    if (firstBuffer.timestamp <= 0 || firstBuffer.sampleRate == 0 ||
        firstBuffer.channelCount == 0 || firstBuffer.bitsPerSample == 0) {
        return true;
    }

    const uint64_t frames = static_cast<uint64_t>(firstBuffer.timestamp) *
                            static_cast<uint64_t>(firstBuffer.sampleRate) / 10000000ULL;
    if (frames == 0) {
        return true;
    }

    AudioBuffer silence;
    silence.timestamp = 0;
    silence.sampleRate = firstBuffer.sampleRate;
    silence.channelCount = firstBuffer.channelCount;
    silence.bitsPerSample = firstBuffer.bitsPerSample;
    silence.sampleCount = static_cast<uint32_t>(
        std::min<uint64_t>(frames, std::numeric_limits<uint32_t>::max()));
    const uint32_t bytesPerSample = silence.bitsPerSample / 8;
    silence.data.resize(static_cast<size_t>(silence.sampleCount) *
                        silence.channelCount * bytesPerSample, 0);

    expectedAudioTimestamp_ = static_cast<int64_t>(silence.sampleCount) * 10000000LL /
                              static_cast<int64_t>(silence.sampleRate);
    return pipeline_->writeAudioSamples(silence);
}

std::unique_ptr<ICaptureSource> RecordingSession::createCaptureSource(CaptureMode mode) {
    switch (mode) {
        case CaptureMode::Screen:
            return std::make_unique<DxgiScreenCapture>();
        case CaptureMode::Window:
            return std::make_unique<WindowCapture>();
        case CaptureMode::Region:
            return std::make_unique<RegionCapture>();
        case CaptureMode::UiElement:
            return std::make_unique<UiElementCapture>();
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
        case AudioDeviceType::ASIO_Output:
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

AudioBuffer RecordingSession::resampleBuffer(const AudioBuffer& input, uint32_t targetRate) {
    uint32_t srcRate = input.sampleRate;
    if (srcRate == targetRate || srcRate == 0) return input;

    double ratio = static_cast<double>(targetRate) / srcRate;
    uint32_t srcFrames = input.sampleCount;
    const uint64_t expectedOutputTotal = static_cast<uint64_t>(
        std::llround(static_cast<double>(resampleInputFramesTotal_ + srcFrames) * ratio));
    uint32_t dstFrames = static_cast<uint32_t>(
        expectedOutputTotal > resampleOutputFramesTotal_
            ? expectedOutputTotal - resampleOutputFramesTotal_
            : 0);
    if (srcFrames > 0 && dstFrames == 0) {
        dstFrames = 1;
    }
    resampleInputFramesTotal_ += srcFrames;
    resampleOutputFramesTotal_ += dstFrames;
    uint32_t channels = input.channelCount;

    AudioBuffer output;
    output.timestamp = input.timestamp;
    output.channelCount = channels;
    output.sampleRate = targetRate;
    output.bitsPerSample = input.bitsPerSample;
    output.sampleCount = dstFrames;

    if (input.bitsPerSample == 16) {
        output.data.resize(dstFrames * channels * 2);
        const int16_t* src = reinterpret_cast<const int16_t*>(input.data.data());
        int16_t* dst = reinterpret_cast<int16_t*>(output.data.data());

        for (uint32_t i = 0; i < dstFrames; ++i) {
            double srcPos = i / ratio;
            uint32_t idx = std::min(static_cast<uint32_t>(srcPos), srcFrames - 1);
            double frac = srcPos - idx;

            for (uint32_t ch = 0; ch < channels; ++ch) {
                int16_t s0 = src[idx * channels + ch];
                int16_t s1 = (idx + 1 < srcFrames)
                    ? src[(idx + 1) * channels + ch] : s0;
                dst[i * channels + ch] = static_cast<int16_t>(
                    s0 + frac * (s1 - s0));
            }
        }
    } else if (input.bitsPerSample == 32) {
        output.data.resize(dstFrames * channels * 4);
        const float* src = reinterpret_cast<const float*>(input.data.data());
        float* dst = reinterpret_cast<float*>(output.data.data());

        for (uint32_t i = 0; i < dstFrames; ++i) {
            double srcPos = i / ratio;
            uint32_t idx = std::min(static_cast<uint32_t>(srcPos), srcFrames - 1);
            double frac = srcPos - idx;

            for (uint32_t ch = 0; ch < channels; ++ch) {
                float s0 = src[idx * channels + ch];
                float s1 = (idx + 1 < srcFrames)
                    ? src[(idx + 1) * channels + ch] : s0;
                dst[i * channels + ch] = static_cast<float>(
                    s0 + frac * (s1 - s0));
            }
        }
    } else {
        // Unsupported bit depth — pass through unchanged
        return input;
    }

    return output;
}

AudioBuffer RecordingSession::normalizeAudioChannels(const AudioBuffer& input, uint32_t targetChannels) {
    if (targetChannels == 0 || input.channelCount == targetChannels) {
        return input;
    }

    const uint32_t srcChannels = input.channelCount;
    const uint32_t bytesPerSample = input.bitsPerSample / 8;
    if (srcChannels == 0 || bytesPerSample == 0) {
        return input;
    }

    AudioBuffer output;
    output.timestamp = input.timestamp;
    output.channelCount = targetChannels;
    output.sampleRate = input.sampleRate;
    output.bitsPerSample = input.bitsPerSample;
    output.sampleCount = input.sampleCount;
    output.data.resize(static_cast<size_t>(input.sampleCount) * targetChannels * bytesPerSample);

    const uint8_t* src = input.data.data();
    uint8_t* dst = output.data.data();
    const uint32_t copyChannels = std::min(srcChannels, targetChannels);

    for (uint32_t frame = 0; frame < input.sampleCount; ++frame) {
        const size_t srcFrame = static_cast<size_t>(frame) * srcChannels * bytesPerSample;
        const size_t dstFrame = static_cast<size_t>(frame) * targetChannels * bytesPerSample;

        for (uint32_t ch = 0; ch < copyChannels; ++ch) {
            std::memcpy(dst + dstFrame + static_cast<size_t>(ch) * bytesPerSample,
                        src + srcFrame + static_cast<size_t>(ch) * bytesPerSample,
                        bytesPerSample);
        }

        for (uint32_t ch = copyChannels; ch < targetChannels; ++ch) {
            const uint32_t sourceCh = copyChannels > 0 ? copyChannels - 1 : 0;
            std::memcpy(dst + dstFrame + static_cast<size_t>(ch) * bytesPerSample,
                        src + srcFrame + static_cast<size_t>(sourceCh) * bytesPerSample,
                        bytesPerSample);
        }
    }

    return output;
}

} // namespace pb
