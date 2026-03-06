#include "pipeline/SinkWriterPipeline.h"

#include <dxgi.h>
#include <stdexcept>

// Link required libraries
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

namespace pb {

// ============================================================================
// Construction / Destruction
// ============================================================================

SinkWriterPipeline::SinkWriterPipeline() = default;

SinkWriterPipeline::~SinkWriterPipeline() {
    if (recording_) {
        stop();
    }
    releaseResources();
    if (mfStarted_) {
        MFShutdown();
        mfStarted_ = false;
    }
}

// ============================================================================
// IRecordingPipeline implementation
// ============================================================================

bool SinkWriterPipeline::initialize(const RecordingConfig& config, ID3D11Device* device) {
    try {
        if (initialized_) {
            releaseResources();
        }

        config_ = config;
        d3dDevice_ = device;
        if (d3dDevice_) {
            d3dDevice_->GetImmediateContext(&d3dContext_);
        }
        hasAudio_ = config_.recordAudio;

        // Initialize Media Foundation
        if (!mfStarted_) {
            PB_CHECK_HR(MFStartup(MF_VERSION, MFSTARTUP_FULL),
                        "MFStartup failed");
            mfStarted_ = true;
        }

        // Setup DXGI device manager for hardware-accelerated encoding
        if (!setupDXGIDeviceManager()) {
            return false;
        }

        // Create Sink Writer attributes
        ComPtr<IMFAttributes> attributes;
        PB_CHECK_HR(MFCreateAttributes(&attributes, 6),
                    "Failed to create SinkWriter attributes");

        PB_CHECK_HR(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE),
                    "Failed to set hardware transforms");

        PB_CHECK_HR(attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE),
                    "Failed to disable throttling");

        // Set the DXGI device manager so encoder can access GPU surfaces directly
        if (dxgiDeviceManager_) {
            PB_CHECK_HR(attributes->SetUnknown(MF_SINK_WRITER_D3D_MANAGER,
                                                dxgiDeviceManager_.Get()),
                        "Failed to set D3D manager on SinkWriter");
        }

        // Create Sink Writer from output URL
        PB_CHECK_HR(MFCreateSinkWriterFromURL(config_.outputPath.c_str(),
                                               nullptr,
                                               attributes.Get(),
                                               &sinkWriter_),
                    "Failed to create SinkWriter from URL");

        // Configure video stream (output then input)
        if (!configureVideoOutput()) return false;
        if (!configureVideoInput()) return false;

        // Try to configure encoder quality/speed via ICodecAPI
        applyEncoderSettings();

        // Configure audio stream if needed
        if (hasAudio_) {
            if (!configureAudioOutput()) return false;
            if (!configureAudioInput()) return false;
        }

        // Reset timing state
        firstVideoTimestamp_.store(-1);
        lastVideoTimestamp_.store(0);
        firstAudioTimestamp_.store(-1);

        initialized_ = true;
        return true;

    } catch (const std::exception& e) {
        reportError(std::string("Initialize failed: ") + e.what());
        return false;
    }
}

bool SinkWriterPipeline::start() {
    try {
        if (!initialized_ || !sinkWriter_) {
            reportError("Pipeline not initialized");
            return false;
        }

        PB_CHECK_HR(sinkWriter_->BeginWriting(), "BeginWriting failed");
        recording_ = true;
        return true;

    } catch (const std::exception& e) {
        reportError(std::string("Start failed: ") + e.what());
        return false;
    }
}

bool SinkWriterPipeline::stop() {
    try {
        if (!recording_) {
            return true;
        }

        recording_ = false;

        {
            std::lock_guard<std::mutex> lock(writeMutex_);
            if (sinkWriter_) {
                // Finalize flushes all buffered data and closes the file
                HRESULT hr = sinkWriter_->Finalize();
                if (FAILED(hr)) {
                    reportError("SinkWriter Finalize failed: " + hrToString(hr));
                }
            }
        }

        releaseResources();
        return true;

    } catch (const std::exception& e) {
        reportError(std::string("Stop failed: ") + e.what());
        return false;
    }
}

bool SinkWriterPipeline::writeVideoFrame(const VideoFrame& frame) {
    if (!recording_ || !sinkWriter_) {
        return false;
    }

    try {
        // Track timing
        int64_t ts = frame.timestamp;
        if (firstVideoTimestamp_.load() < 0) {
            firstVideoTimestamp_.store(ts);
        }
        int64_t relativeTs = ts - firstVideoTimestamp_.load();
        if (relativeTs < 0) relativeTs = 0;

        lastVideoTimestamp_.store(ts);

        // Duration of one frame in 100ns units
        int64_t frameDuration = 10000000LL / config_.video.fps;

        // Ensure staging texture exists for GPU→CPU copy
        if (!ensureStagingTexture(frame.width, frame.height)) {
            reportError("Failed to create staging texture");
            return false;
        }

        // Copy GPU texture to staging texture
        d3dContext_->CopyResource(stagingTexture_.Get(), frame.texture.Get());

        // Map staging texture to read pixels
        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = d3dContext_->Map(stagingTexture_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            reportError("Failed to map staging texture: " + hrToString(hr));
            return false;
        }

        // Calculate buffer size: width * height * 4 bytes (BGRA)
        DWORD stride = frame.width * 4;
        DWORD dataSize = stride * frame.height;

        // Create MF memory buffer
        ComPtr<IMFMediaBuffer> mediaBuffer;
        PB_CHECK_HR(MFCreateMemoryBuffer(dataSize, &mediaBuffer),
                    "Failed to create video memory buffer");

        // Copy pixel data (handle stride mismatch)
        BYTE* bufferPtr = nullptr;
        PB_CHECK_HR(mediaBuffer->Lock(&bufferPtr, nullptr, nullptr),
                    "Failed to lock video buffer");

        if (mapped.RowPitch == stride) {
            memcpy(bufferPtr, mapped.pData, dataSize);
        } else {
            // Row-by-row copy for stride mismatch
            const BYTE* src = static_cast<const BYTE*>(mapped.pData);
            for (uint32_t y = 0; y < frame.height; ++y) {
                memcpy(bufferPtr + y * stride, src + y * mapped.RowPitch, stride);
            }
        }

        d3dContext_->Unmap(stagingTexture_.Get(), 0);

        PB_CHECK_HR(mediaBuffer->Unlock(), "Failed to unlock video buffer");
        PB_CHECK_HR(mediaBuffer->SetCurrentLength(dataSize),
                    "Failed to set video buffer length");

        // Create the sample
        ComPtr<IMFSample> sample;
        PB_CHECK_HR(MFCreateSample(&sample), "Failed to create video sample");
        PB_CHECK_HR(sample->AddBuffer(mediaBuffer.Get()), "Failed to add video buffer");
        PB_CHECK_HR(sample->SetSampleTime(relativeTs), "Failed to set video sample time");
        PB_CHECK_HR(sample->SetSampleDuration(frameDuration), "Failed to set video duration");

        // Write under lock
        {
            std::lock_guard<std::mutex> lock(writeMutex_);
            if (!recording_) return false;
            PB_CHECK_HR(sinkWriter_->WriteSample(videoStreamIndex_, sample.Get()),
                        "WriteSample video failed");
        }

        return true;

    } catch (const std::exception& e) {
        reportError(std::string("writeVideoFrame: ") + e.what());
        return false;
    }
}

bool SinkWriterPipeline::ensureStagingTexture(uint32_t width, uint32_t height) {
    if (stagingTexture_ && stagingWidth_ == width && stagingHeight_ == height) {
        return true;
    }

    stagingTexture_.Reset();

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    HRESULT hr = d3dDevice_->CreateTexture2D(&desc, nullptr, &stagingTexture_);
    if (FAILED(hr)) {
        return false;
    }

    stagingWidth_ = width;
    stagingHeight_ = height;
    return true;
}

bool SinkWriterPipeline::writeAudioSamples(const AudioBuffer& buffer) {
    if (!recording_ || !sinkWriter_ || !hasAudio_) {
        return false;
    }

    try {
        int64_t ts = buffer.timestamp;
        if (firstAudioTimestamp_.load() < 0) {
            firstAudioTimestamp_.store(ts);
        }
        int64_t relativeTs = ts - firstAudioTimestamp_.load();
        if (relativeTs < 0) relativeTs = 0;

        // Duration in 100ns units: (sampleCount / sampleRate) * 10,000,000
        int64_t duration = 0;
        if (buffer.sampleRate > 0) {
            duration = static_cast<int64_t>(buffer.sampleCount) * 10000000LL
                       / static_cast<int64_t>(buffer.sampleRate);
        }

        DWORD dataSize = static_cast<DWORD>(buffer.data.size());
        if (dataSize == 0) {
            return true; // Nothing to write
        }

        // Create memory buffer
        ComPtr<IMFMediaBuffer> mediaBuffer;
        PB_CHECK_HR(MFCreateMemoryBuffer(dataSize, &mediaBuffer),
                    "Failed to create audio memory buffer");

        // Copy PCM data into the buffer
        BYTE* bufferPtr = nullptr;
        PB_CHECK_HR(mediaBuffer->Lock(&bufferPtr, nullptr, nullptr),
                    "Failed to lock audio buffer");
        memcpy(bufferPtr, buffer.data.data(), dataSize);
        PB_CHECK_HR(mediaBuffer->Unlock(), "Failed to unlock audio buffer");
        PB_CHECK_HR(mediaBuffer->SetCurrentLength(dataSize),
                    "Failed to set audio buffer length");

        // Create the sample
        ComPtr<IMFSample> sample;
        PB_CHECK_HR(MFCreateSample(&sample), "Failed to create audio sample");
        PB_CHECK_HR(sample->AddBuffer(mediaBuffer.Get()), "Failed to add audio buffer");
        PB_CHECK_HR(sample->SetSampleTime(relativeTs), "Failed to set audio sample time");
        PB_CHECK_HR(sample->SetSampleDuration(duration), "Failed to set audio duration");

        // Write under lock
        {
            std::lock_guard<std::mutex> lock(writeMutex_);
            if (!recording_) return false;
            PB_CHECK_HR(sinkWriter_->WriteSample(audioStreamIndex_, sample.Get()),
                        "WriteSample audio failed");
        }

        return true;

    } catch (const std::exception& e) {
        reportError(std::string("writeAudioSamples failed: ") + e.what());
        return false;
    }
}

void SinkWriterPipeline::setErrorCallback(ErrorCallback callback) {
    errorCallback_ = std::move(callback);
}

bool SinkWriterPipeline::isRecording() const {
    return recording_;
}

int64_t SinkWriterPipeline::getDurationMs() const {
    int64_t first = firstVideoTimestamp_.load();
    int64_t last = lastVideoTimestamp_.load();
    if (first < 0 || last <= first) {
        return 0;
    }
    // Timestamps are in 100ns units, convert to ms
    return (last - first) / 10000;
}

int64_t SinkWriterPipeline::getFileSize() const {
    HANDLE hFile = CreateFileW(config_.outputPath.c_str(),
                               GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr,
                               OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL,
                               nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return 0;
    }

    LARGE_INTEGER fileSize;
    BOOL ok = GetFileSizeEx(hFile, &fileSize);
    CloseHandle(hFile);

    return ok ? fileSize.QuadPart : 0;
}

// ============================================================================
// Private helpers
// ============================================================================

bool SinkWriterPipeline::configureVideoOutput() {
    ComPtr<IMFMediaType> outputType;
    PB_CHECK_HR(MFCreateMediaType(&outputType), "Failed to create video output type");

    PB_CHECK_HR(outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video),
                "Failed to set video major type");

    int width  = config_.video.width;
    int height = config_.video.height;

    // Select codec-specific output format
    switch (config_.video.codec) {
    case VideoCodec::H264:
        PB_CHECK_HR(outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264),
                    "Failed to set H264 subtype");

        // Profile: Main for general compatibility, High for better quality
        {
            UINT32 profile = (config_.video.quality >= 80)
                             ? eAVEncH264VProfile_High
                             : eAVEncH264VProfile_Main;
            PB_CHECK_HR(outputType->SetUINT32(MF_MT_MPEG2_PROFILE, profile),
                        "Failed to set H264 profile");
            // Level: 0 means auto
            PB_CHECK_HR(outputType->SetUINT32(MF_MT_MPEG2_LEVEL, 0),
                        "Failed to set H264 level");
        }
        break;

    case VideoCodec::WMV:
        PB_CHECK_HR(outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_WMV3),
                    "Failed to set WMV3 subtype");
        break;
    }

    PB_CHECK_HR(outputType->SetUINT32(MF_MT_AVG_BITRATE, config_.video.bitrate),
                "Failed to set video bitrate");

    PB_CHECK_HR(MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, width, height),
                "Failed to set video frame size");

    PB_CHECK_HR(MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, config_.video.fps, 1),
                "Failed to set video frame rate");

    PB_CHECK_HR(MFSetAttributeRatio(outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1),
                "Failed to set pixel aspect ratio");

    PB_CHECK_HR(outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive),
                "Failed to set interlace mode");

    PB_CHECK_HR(sinkWriter_->AddStream(outputType.Get(), &videoStreamIndex_),
                "Failed to add video output stream");

    return true;
}

bool SinkWriterPipeline::configureVideoInput() {
    ComPtr<IMFMediaType> inputType;
    PB_CHECK_HR(MFCreateMediaType(&inputType), "Failed to create video input type");

    PB_CHECK_HR(inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video),
                "Failed to set video input major type");

    // DXGI Desktop Duplication captures in BGRA.
    // Media Foundation's MFVideoFormat_RGB32 is actually BGRA in memory layout,
    // which matches the DXGI_FORMAT_B8G8R8A8_UNORM from Desktop Duplication.
    PB_CHECK_HR(inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32),
                "Failed to set video input subtype (RGB32/BGRA)");

    int width  = config_.video.width;
    int height = config_.video.height;

    PB_CHECK_HR(MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, width, height),
                "Failed to set video input frame size");

    PB_CHECK_HR(MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, config_.video.fps, 1),
                "Failed to set video input frame rate");

    PB_CHECK_HR(MFSetAttributeRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1),
                "Failed to set video input pixel aspect ratio");

    PB_CHECK_HR(inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive),
                "Failed to set video input interlace mode");

    PB_CHECK_HR(sinkWriter_->SetInputMediaType(videoStreamIndex_, inputType.Get(), nullptr),
                "Failed to set video input media type");

    return true;
}

bool SinkWriterPipeline::configureAudioOutput() {
    ComPtr<IMFMediaType> outputType;
    PB_CHECK_HR(MFCreateMediaType(&outputType), "Failed to create audio output type");

    PB_CHECK_HR(outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio),
                "Failed to set audio major type");

    int sampleRate = config_.audio.sampleRate;
    int channels   = config_.audio.channelCount;
    int bitrate    = config_.audio.bitrate;

    switch (config_.audio.codec) {
    case AudioCodec::AAC:
        PB_CHECK_HR(outputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC),
                    "Failed to set AAC subtype");
        PB_CHECK_HR(outputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16),
                    "Failed to set AAC bits per sample");
        PB_CHECK_HR(outputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate),
                    "Failed to set AAC sample rate");
        PB_CHECK_HR(outputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels),
                    "Failed to set AAC channels");
        PB_CHECK_HR(outputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bitrate / 8),
                    "Failed to set AAC bitrate");
        break;

    case AudioCodec::MP3:
        PB_CHECK_HR(outputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_MP3),
                    "Failed to set MP3 subtype");
        PB_CHECK_HR(outputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate),
                    "Failed to set MP3 sample rate");
        PB_CHECK_HR(outputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels),
                    "Failed to set MP3 channels");
        PB_CHECK_HR(outputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bitrate / 8),
                    "Failed to set MP3 bitrate");
        break;

    case AudioCodec::WMA:
        PB_CHECK_HR(outputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_WMAudioV9),
                    "Failed to set WMA subtype");
        PB_CHECK_HR(outputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate),
                    "Failed to set WMA sample rate");
        PB_CHECK_HR(outputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels),
                    "Failed to set WMA channels");
        PB_CHECK_HR(outputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bitrate / 8),
                    "Failed to set WMA bitrate");
        break;

    default:
        // PCM, Opus, Vorbis are not supported in Sink Writer natively
        reportError("Unsupported audio codec for SinkWriter pipeline");
        return false;
    }

    PB_CHECK_HR(sinkWriter_->AddStream(outputType.Get(), &audioStreamIndex_),
                "Failed to add audio output stream");

    return true;
}

bool SinkWriterPipeline::configureAudioInput() {
    ComPtr<IMFMediaType> inputType;
    PB_CHECK_HR(MFCreateMediaType(&inputType), "Failed to create audio input type");

    PB_CHECK_HR(inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio),
                "Failed to set audio input major type");

    PB_CHECK_HR(inputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM),
                "Failed to set audio input subtype (PCM)");

    int sampleRate   = config_.audio.sampleRate;
    int channels     = config_.audio.channelCount;
    int bitsPerSample = config_.audio.bitsPerSample;
    int blockAlign   = channels * (bitsPerSample / 8);

    PB_CHECK_HR(inputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate),
                "Failed to set audio input sample rate");

    PB_CHECK_HR(inputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels),
                "Failed to set audio input channels");

    PB_CHECK_HR(inputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, bitsPerSample),
                "Failed to set audio input bits per sample");

    PB_CHECK_HR(inputType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, blockAlign),
                "Failed to set audio input block alignment");

    PB_CHECK_HR(inputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                                      sampleRate * blockAlign),
                "Failed to set audio input avg bytes per second");

    PB_CHECK_HR(sinkWriter_->SetInputMediaType(audioStreamIndex_, inputType.Get(), nullptr),
                "Failed to set audio input media type");

    return true;
}

bool SinkWriterPipeline::setupDXGIDeviceManager() {
    if (!d3dDevice_) {
        // No D3D device provided; software encoding path
        return true;
    }

    PB_CHECK_HR(MFCreateDXGIDeviceManager(&resetToken_, &dxgiDeviceManager_),
                "Failed to create DXGI device manager");

    PB_CHECK_HR(dxgiDeviceManager_->ResetDevice(d3dDevice_.Get(), resetToken_),
                "Failed to reset DXGI device manager with D3D11 device");

    return true;
}

void SinkWriterPipeline::applyEncoderSettings() {
    // Apply encoder parameters via IMFAttributes on SetInputMediaType's
    // encoding parameters. For non-realtime mode, we set low-latency attributes.
    // The actual quality vs speed tradeoff is primarily controlled by:
    // - MF_SINK_WRITER_DISABLE_THROTTLING (set during init)
    // - The encoding profile (set in configureVideoOutput)
    //
    // Additional encoder tuning via ICodecAPI is not available on MinGW,
    // so we rely on the SinkWriter-level controls.

    if (!config_.video.realtimeEncode) {
        // For non-realtime encoding, try to set encoder parameters via attributes
        ComPtr<IMFAttributes> encoderParams;
        HRESULT hr = MFCreateAttributes(&encoderParams, 1);
        if (SUCCEEDED(hr)) {
            // CODECAPI_AVLowLatencyMode GUID
            static const GUID AVLowLatencyMode =
                {0x9c27891a, 0xed7a, 0x40e1, {0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee}};
            encoderParams->SetUINT32(AVLowLatencyMode, FALSE);

            // Re-set input media type with encoder parameters
            // Create input type again
            ComPtr<IMFMediaType> inputType;
            hr = MFCreateMediaType(&inputType);
            if (SUCCEEDED(hr)) {
                inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
                MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE,
                                   config_.video.width, config_.video.height);
                MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE,
                                    config_.video.fps, 1);
                MFSetAttributeRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
                inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

                sinkWriter_->SetInputMediaType(videoStreamIndex_,
                                                inputType.Get(),
                                                encoderParams.Get());
                // Silently ignore failure - encoding will still work with defaults
            }
        }
    }
}

void SinkWriterPipeline::reportError(const std::string& msg) {
    if (errorCallback_) {
        errorCallback_(msg);
    }
}

void SinkWriterPipeline::releaseResources() {
    sinkWriter_.Reset();
    dxgiDeviceManager_.Reset();
    stagingTexture_.Reset();
    d3dContext_.Reset();
    initialized_ = false;
}

} // namespace pb
