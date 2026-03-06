#pragma once

#include "pipeline/IRecordingPipeline.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <mftransform.h>
#include <codecapi.h>

#include <mutex>
#include <atomic>

namespace pb {

class SinkWriterPipeline : public IRecordingPipeline {
public:
    SinkWriterPipeline();
    ~SinkWriterPipeline() override;

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
    // Media type configuration helpers
    bool configureVideoOutput();
    bool configureVideoInput();
    bool configureAudioOutput();
    bool configureAudioInput();
    bool setupDXGIDeviceManager();
    void applyEncoderSettings();

    void reportError(const std::string& msg);
    void releaseResources();

    RecordingConfig config_;
    ComPtr<ID3D11Device> d3dDevice_;
    ComPtr<IMFSinkWriter> sinkWriter_;
    ComPtr<IMFDXGIDeviceManager> dxgiDeviceManager_;

    DWORD videoStreamIndex_ = 0;
    DWORD audioStreamIndex_ = 0;
    UINT resetToken_ = 0;

    std::atomic<bool> recording_{false};
    std::atomic<bool> initialized_{false};
    bool mfStarted_ = false;
    bool hasAudio_ = false;

    // Timing
    std::atomic<int64_t> firstVideoTimestamp_{-1};
    std::atomic<int64_t> lastVideoTimestamp_{0};
    std::atomic<int64_t> firstAudioTimestamp_{-1};

    // Staging texture for GPU→CPU copy
    ComPtr<ID3D11Texture2D> stagingTexture_;
    ComPtr<ID3D11DeviceContext> d3dContext_;
    uint32_t stagingWidth_ = 0;
    uint32_t stagingHeight_ = 0;
    bool ensureStagingTexture(uint32_t width, uint32_t height);

    // Thread safety
    std::mutex writeMutex_;

    ErrorCallback errorCallback_;
};

} // namespace pb
