#include "pipeline/H264MftEncoder.h"

#include <codecapi.h>
#include <icodecapi.h>
#include <mferror.h>

#include <algorithm>
#include <cstring>

namespace pb {

namespace {

constexpr GUID PB_CLSID_CMSH264EncoderMFT =
    {0x6ca50344, 0x051a, 0x4ded, {0x97, 0x79, 0xa4, 0x33, 0x05, 0x16, 0x5e, 0x35}};

uint8_t clampByte(int value)
{
    return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

void bgraToNv12(const uint8_t* bgra,
                uint32_t width,
                uint32_t height,
                uint32_t stride,
                std::vector<uint8_t>& nv12)
{
    const size_t ySize = static_cast<size_t>(width) * height;
    nv12.assign(ySize + ySize / 2, 0);

    uint8_t* yPlane = nv12.data();
    uint8_t* uvPlane = nv12.data() + ySize;

    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = bgra + static_cast<size_t>(y) * stride;
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t b = row[x * 4 + 0];
            const uint8_t g = row[x * 4 + 1];
            const uint8_t r = row[x * 4 + 2];
            const int yy = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            yPlane[static_cast<size_t>(y) * width + x] = clampByte(yy);
        }
    }

    for (uint32_t y = 0; y + 1 < height; y += 2) {
        for (uint32_t x = 0; x + 1 < width; x += 2) {
            int uSum = 0;
            int vSum = 0;
            for (uint32_t dy = 0; dy < 2; ++dy) {
                const uint8_t* row = bgra + static_cast<size_t>(y + dy) * stride;
                for (uint32_t dx = 0; dx < 2; ++dx) {
                    const uint8_t b = row[(x + dx) * 4 + 0];
                    const uint8_t g = row[(x + dx) * 4 + 1];
                    const uint8_t r = row[(x + dx) * 4 + 2];
                    uSum += ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                    vSum += ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
                }
            }
            const size_t uvIndex = static_cast<size_t>(y / 2) * width + x;
            uvPlane[uvIndex + 0] = clampByte(uSum / 4);
            uvPlane[uvIndex + 1] = clampByte(vSum / 4);
        }
    }
}

} // namespace

H264MftEncoder::~H264MftEncoder()
{
    shutdown();
}

bool H264MftEncoder::initialize(const RecordingConfig& config, ID3D11Device* device)
{
    config_ = config;
    d3dDevice_ = device;
    lastError_.clear();
    if (d3dDevice_) {
        d3dDevice_->GetImmediateContext(&d3dContext_);
    }

    HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(coHr)) {
        comInitialized_ = true;
    } else if (coHr != RPC_E_CHANGED_MODE) {
        lastError_ = "CoInitializeEx failed: " + hrToString(coHr);
        return false;
    }

    if (!mfStarted_) {
        HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        if (FAILED(hr)) {
            lastError_ = "MFStartup failed: " + hrToString(hr);
            return false;
        }
        mfStarted_ = true;
    }

    if (!createEncoder()) {
        return false;
    }

    if (!configureOutputType() || !configureInputType()) {
        return false;
    }

    Microsoft::WRL::ComPtr<ICodecAPI> codecApi;
    if (SUCCEEDED(encoder_->QueryInterface(IID_ICodecAPI,
                                           reinterpret_cast<void**>(codecApi.GetAddressOf())))) {
        VARIANT value;
        VariantInit(&value);
        value.vt = VT_BOOL;
        value.boolVal = config_.video.realtimeEncode ? VARIANT_TRUE : VARIANT_FALSE;
        codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &value);
        codecApi->SetValue(&CODECAPI_AVEncCommonRealTime, &value);
        VariantClear(&value);

        VariantInit(&value);
        value.vt = VT_UI4;
        value.ulVal = 0;
        codecApi->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &value);
        VariantClear(&value);
    }

    encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    firstTimestamp_ = -1;
    initialized_ = true;
    return true;
}

bool H264MftEncoder::createEncoder()
{
    MFT_REGISTER_TYPE_INFO inputInfo = {};
    inputInfo.guidMajorType = MFMediaType_Video;
    inputInfo.guidSubtype = MFVideoFormat_NV12;

    MFT_REGISTER_TYPE_INFO outputInfo = {};
    outputInfo.guidMajorType = MFMediaType_Video;
    outputInfo.guidSubtype = MFVideoFormat_H264;

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                   MFT_ENUM_FLAG_SYNCMFT |
                       MFT_ENUM_FLAG_LOCALMFT |
                       MFT_ENUM_FLAG_SORTANDFILTER,
                   &inputInfo,
                   &outputInfo,
                   &activates,
                   &count);
    if (SUCCEEDED(hr) && count > 0 && activates) {
        for (UINT32 i = 0; i < count && !encoder_; ++i) {
            if (activates[i]) {
                hr = activates[i]->ActivateObject(IID_PPV_ARGS(&encoder_));
                if (FAILED(hr)) {
                    lastError_ = "H.264 MFT ActivateObject failed: " + hrToString(hr);
                }
            }
        }

        for (UINT32 i = 0; i < count; ++i) {
            if (activates[i]) {
                activates[i]->Release();
            }
        }
        CoTaskMemFree(activates);
    } else {
        lastError_ = "MFTEnumEx H.264 encoder failed: " + hrToString(hr);
        if (activates) {
            CoTaskMemFree(activates);
        }
    }

    if (encoder_) {
        return true;
    }

    hr = CoCreateInstance(PB_CLSID_CMSH264EncoderMFT,
                          nullptr,
                          CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&encoder_));
    if (SUCCEEDED(hr) && encoder_) {
        return true;
    }

    if (!lastError_.empty()) {
        lastError_ += "; ";
    }
    lastError_ += "CoCreateInstance H.264 MFT failed: " + hrToString(hr);

    return false;
}

bool H264MftEncoder::configureOutputType()
{
    HRESULT lastHr = E_FAIL;
    for (DWORD i = 0; ; ++i) {
        Microsoft::WRL::ComPtr<IMFMediaType> type;
        HRESULT hr = encoder_->GetOutputAvailableType(0, i, &type);
        if (hr == MF_E_NO_MORE_TYPES) {
            break;
        }
        if (FAILED(hr) || !type) {
            lastHr = hr;
            continue;
        }

        GUID subtype = {};
        if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) ||
            !IsEqualGUID(subtype, MFVideoFormat_H264)) {
            continue;
        }

        type->SetUINT32(MF_MT_AVG_BITRATE, config_.video.bitrate);
        MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE,
                           config_.video.width, config_.video.height);
        MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, config_.video.fps, 1);
        MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);

        hr = encoder_->SetOutputType(0, type.Get(), 0);
        if (SUCCEEDED(hr)) {
            return true;
        }
        lastHr = hr;
    }

    Microsoft::WRL::ComPtr<IMFMediaType> type;
    PB_CHECK_HR(MFCreateMediaType(&type), "Failed to create H.264 output type");
    PB_CHECK_HR(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Failed to set major type");
    PB_CHECK_HR(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264), "Failed to set H.264 subtype");
    PB_CHECK_HR(type->SetUINT32(MF_MT_AVG_BITRATE, config_.video.bitrate), "Failed to set bitrate");
    PB_CHECK_HR(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE,
                                   config_.video.width, config_.video.height),
                "Failed to set frame size");
    PB_CHECK_HR(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, config_.video.fps, 1),
                "Failed to set frame rate");
    PB_CHECK_HR(MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1),
                "Failed to set pixel aspect ratio");
    PB_CHECK_HR(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive),
                "Failed to set interlace mode");

    HRESULT hr = encoder_->SetOutputType(0, type.Get(), 0);
    if (FAILED(hr)) {
        lastError_ = "SetOutputType H.264 failed: " + hrToString(hr) +
                     " (available type last result: " + hrToString(lastHr) + ")";
        return false;
    }
    return true;
}

bool H264MftEncoder::configureInputType()
{
    Microsoft::WRL::ComPtr<IMFMediaType> type;
    PB_CHECK_HR(MFCreateMediaType(&type), "Failed to create H.264 input type");
    PB_CHECK_HR(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Failed to set major type");
    PB_CHECK_HR(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12), "Failed to set NV12 subtype");
    PB_CHECK_HR(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE,
                                   config_.video.width, config_.video.height),
                "Failed to set frame size");
    PB_CHECK_HR(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, config_.video.fps, 1),
                "Failed to set frame rate");
    PB_CHECK_HR(MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1),
                "Failed to set pixel aspect ratio");
    PB_CHECK_HR(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive),
                "Failed to set interlace mode");
    PB_CHECK_HR(type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE),
                "Failed to set independent samples");
    PB_CHECK_HR(type->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE),
                "Failed to set fixed samples");
    PB_CHECK_HR(type->SetUINT32(MF_MT_SAMPLE_SIZE,
                                static_cast<UINT32>(
                                    static_cast<uint64_t>(config_.video.width) *
                                    static_cast<uint64_t>(config_.video.height) * 3ULL / 2ULL)),
                "Failed to set sample size");
    PB_CHECK_HR(type->SetUINT32(MF_MT_DEFAULT_STRIDE, config_.video.width),
                "Failed to set default stride");

    HRESULT hr = encoder_->SetInputType(0, type.Get(), 0);
    if (FAILED(hr)) {
        lastError_ = "SetInputType NV12 failed: " + hrToString(hr);
        return false;
    }
    return true;
}

bool H264MftEncoder::encodeFrame(const VideoFrame& frame, std::vector<EncodedVideoPacket>& packets)
{
    if (!initialized_ || !encoder_) {
        return false;
    }

    std::vector<uint8_t> nv12;
    if (!copyFrameToNv12(frame, nv12)) {
        return false;
    }

    int64_t relativeTs = frame.timestamp;
    if (relativeTs < 0) relativeTs = 0;
    const int64_t duration = 10000000LL / config_.video.fps;

    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    PB_CHECK_HR(MFCreateMemoryBuffer(static_cast<DWORD>(nv12.size()), &buffer),
                "Failed to create NV12 input buffer");
    BYTE* dst = nullptr;
    PB_CHECK_HR(buffer->Lock(&dst, nullptr, nullptr), "Failed to lock NV12 buffer");
    std::memcpy(dst, nv12.data(), nv12.size());
    PB_CHECK_HR(buffer->Unlock(), "Failed to unlock NV12 buffer");
    PB_CHECK_HR(buffer->SetCurrentLength(static_cast<DWORD>(nv12.size())),
                "Failed to set NV12 length");

    Microsoft::WRL::ComPtr<IMFSample> sample;
    PB_CHECK_HR(MFCreateSample(&sample), "Failed to create H.264 input sample");
    PB_CHECK_HR(sample->AddBuffer(buffer.Get()), "Failed to add H.264 input buffer");
    PB_CHECK_HR(sample->SetSampleTime(relativeTs), "Failed to set H.264 input time");
    PB_CHECK_HR(sample->SetSampleDuration(duration), "Failed to set H.264 input duration");

    HRESULT hr = encoder_->ProcessInput(0, sample.Get(), 0);
    if (FAILED(hr)) {
        return false;
    }

    return processOutput(packets);
}

bool H264MftEncoder::drain(std::vector<EncodedVideoPacket>& packets)
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

bool H264MftEncoder::processOutput(std::vector<EncodedVideoPacket>& packets)
{
    MFT_OUTPUT_STREAM_INFO info = {};
    HRESULT hr = encoder_->GetOutputStreamInfo(0, &info);
    if (FAILED(hr)) {
        return false;
    }

    for (;;) {
        Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
        PB_CHECK_HR(MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize, 1024 * 1024), &buffer),
                    "Failed to create H.264 output buffer");
        Microsoft::WRL::ComPtr<IMFSample> sample;
        PB_CHECK_HR(MFCreateSample(&sample), "Failed to create H.264 output sample");
        PB_CHECK_HR(sample->AddBuffer(buffer.Get()), "Failed to add H.264 output buffer");

        MFT_OUTPUT_DATA_BUFFER output = {};
        output.dwStreamID = 0;
        output.pSample = sample.Get();
        DWORD status = 0;
        hr = encoder_->ProcessOutput(0, 1, &output, &status);

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            return true;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            configureOutputType();
            continue;
        }
        if (FAILED(hr)) {
            return false;
        }

        Microsoft::WRL::ComPtr<IMFMediaBuffer> contiguous;
        PB_CHECK_HR(sample->ConvertToContiguousBuffer(&contiguous),
                    "Failed to get H.264 output buffer");
        BYTE* data = nullptr;
        DWORD length = 0;
        PB_CHECK_HR(contiguous->Lock(&data, nullptr, &length), "Failed to lock H.264 output");

        if (length > 0) {
            LONGLONG sampleTime = 0;
            LONGLONG sampleDuration = 0;
            sample->GetSampleTime(&sampleTime);
            sample->GetSampleDuration(&sampleDuration);
            UINT32 cleanPoint = 0;
            sample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint);

            EncodedVideoPacket packet;
            packet.data.assign(data, data + length);
            packet.timestampMs = sampleTime / 10000;
            packet.durationMs = sampleDuration / 10000;
            packet.keyframe = cleanPoint != 0;
            packets.push_back(std::move(packet));
        }

        contiguous->Unlock();

        if (!(output.dwStatus & MFT_OUTPUT_DATA_BUFFER_INCOMPLETE)) {
            return true;
        }
    }
}

bool H264MftEncoder::ensureStagingTexture(uint32_t width, uint32_t height)
{
    if (stagingTexture_ && stagingWidth_ == width && stagingHeight_ == height) {
        return true;
    }
    if (!d3dDevice_) {
        return false;
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

bool H264MftEncoder::copyFrameToNv12(const VideoFrame& frame, std::vector<uint8_t>& nv12)
{
    if (!frame.texture || !d3dContext_) {
        return false;
    }

    const uint32_t encWidth = static_cast<uint32_t>(config_.video.width);
    const uint32_t encHeight = static_cast<uint32_t>(config_.video.height);
    if (!ensureStagingTexture(frame.width, frame.height)) {
        return false;
    }

    d3dContext_->CopyResource(stagingTexture_.Get(), frame.texture.Get());

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = d3dContext_->Map(stagingTexture_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        return false;
    }

    std::vector<uint8_t> padded(static_cast<size_t>(encWidth) * encHeight * 4, 0);
    const uint32_t copyWidth = std::min(frame.width, encWidth);
    const uint32_t copyHeight = std::min(frame.height, encHeight);
    const BYTE* src = static_cast<const BYTE*>(mapped.pData);
    for (uint32_t y = 0; y < copyHeight; ++y) {
        std::memcpy(padded.data() + static_cast<size_t>(y) * encWidth * 4,
                    src + static_cast<size_t>(y) * mapped.RowPitch,
                    static_cast<size_t>(copyWidth) * 4);
    }

    d3dContext_->Unmap(stagingTexture_.Get(), 0);
    bgraToNv12(padded.data(), encWidth, encHeight, encWidth * 4, nv12);
    return true;
}

void H264MftEncoder::shutdown()
{
    if (encoder_) {
        encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }
    encoder_.Reset();
    stagingTexture_.Reset();
    d3dContext_.Reset();
    d3dDevice_.Reset();
    stagingWidth_ = 0;
    stagingHeight_ = 0;
    initialized_ = false;
    if (mfStarted_) {
        MFShutdown();
        mfStarted_ = false;
    }
    if (comInitialized_) {
        CoUninitialize();
        comInitialized_ = false;
    }
}

} // namespace pb
