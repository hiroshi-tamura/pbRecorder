#include "WasapiCapture.h"

#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <stdexcept>


namespace pb {

// ============================================================================
// Helper: Safe COM release
// ============================================================================
template <typename T>
static void safeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// ============================================================================
// Helper: Scoped COM initializer (per-thread)
// ============================================================================
struct ScopedCom {
    HRESULT hr;
    ScopedCom() { hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~ScopedCom() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
    bool ok() const { return SUCCEEDED(hr); }
};

// ============================================================================
// Constructor / Destructor
// ============================================================================
WasapiCapture::WasapiCapture() {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    qpcFrequency_ = freq.QuadPart;
}

WasapiCapture::~WasapiCapture() {
    stop();
    releaseResources();
}

// ============================================================================
// enumerateDevices
// ============================================================================
std::vector<AudioDeviceInfo> WasapiCapture::enumerateDevices() {
    std::vector<AudioDeviceInfo> result;

    ScopedCom com;
    if (!com.ok()) return result;

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || !enumerator) return result;

    // Lambda to enumerate devices of a given data flow
    auto enumerate = [&](EDataFlow flow, AudioDeviceType type) {
        IMMDeviceCollection* collection = nullptr;
        hr = enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection);
        if (FAILED(hr) || !collection) return;

        UINT count = 0;
        collection->GetCount(&count);

        for (UINT i = 0; i < count; ++i) {
            IMMDevice* device = nullptr;
            if (FAILED(collection->Item(i, &device)) || !device) continue;

            // Get device ID
            LPWSTR deviceId = nullptr;
            if (FAILED(device->GetId(&deviceId))) {
                safeRelease(device);
                continue;
            }

            // Get friendly name from property store
            IPropertyStore* props = nullptr;
            std::wstring friendlyName = L"Unknown Device";
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) && props) {
                PROPVARIANT varName;
                PropVariantInit(&varName);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &varName))) {
                    if (varName.vt == VT_LPWSTR && varName.pwszVal) {
                        friendlyName = varName.pwszVal;
                    }
                    PropVariantClear(&varName);
                }
                safeRelease(props);
            }

            // Get default format via IAudioClient
            IAudioClient* client = nullptr;
            int channels = 2;
            int rate = 48000;
            int bits = 16;
            if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                           nullptr, reinterpret_cast<void**>(&client))) && client) {
                WAVEFORMATEX* fmt = nullptr;
                if (SUCCEEDED(client->GetMixFormat(&fmt)) && fmt) {
                    channels = fmt->nChannels;
                    rate = fmt->nSamplesPerSec;
                    // Mix format is usually float32, report as 16-bit for general use
                    bits = 16;
                    if (fmt->wBitsPerSample == 16) bits = 16;
                    else if (fmt->wBitsPerSample == 32) bits = 32;
                    CoTaskMemFree(fmt);
                }
                safeRelease(client);
            }

            AudioDeviceInfo info;
            info.id = deviceId;
            info.name = friendlyName;
            info.type = type;
            info.channelCount = channels;
            info.sampleRate = rate;
            info.bitsPerSample = bits;
            result.push_back(std::move(info));

            CoTaskMemFree(deviceId);
            safeRelease(device);
        }

        safeRelease(collection);
    };

    // Render endpoints (for loopback capture of system audio)
    enumerate(eRender, AudioDeviceType::WASAPI_Render);

    // Capture endpoints (microphones)
    enumerate(eCapture, AudioDeviceType::WASAPI_Capture);

    safeRelease(enumerator);
    return result;
}

// ============================================================================
// initialize
// ============================================================================
bool WasapiCapture::initialize(const AudioDeviceInfo& device) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (capturing_) return false;

    releaseResources();

    try {
        if (!initializeDevice(device)) return false;
        initialized_ = true;
        return true;
    } catch (const std::exception& e) {
        reportError(std::string("WasapiCapture::initialize failed: ") + e.what());
        releaseResources();
        return false;
    }
}

// ============================================================================
// initializeDevice
// ============================================================================
bool WasapiCapture::initializeDevice(const AudioDeviceInfo& device) {
    deviceInfo_ = device;
    isLoopback_ = (device.type == AudioDeviceType::WASAPI_Render);

    // Create event handles
    eventHandle_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!eventHandle_ || !stopEvent_) {
        reportError("Failed to create event handles");
        return false;
    }

    // Create device enumerator
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr)) {
        reportError("Failed to create device enumerator: " + hrToString(hr));
        return false;
    }

    // Get the device by ID
    hr = enumerator->GetDevice(device.id.c_str(), &device_);
    safeRelease(enumerator);
    if (FAILED(hr) || !device_) {
        reportError("Failed to get audio device: " + hrToString(hr));
        return false;
    }

    // Activate IAudioClient
    hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                           nullptr, reinterpret_cast<void**>(&audioClient_));
    if (FAILED(hr) || !audioClient_) {
        reportError("Failed to activate IAudioClient: " + hrToString(hr));
        return false;
    }

    // Get mix format
    hr = audioClient_->GetMixFormat(&mixFormat_);
    if (FAILED(hr) || !mixFormat_) {
        reportError("Failed to get mix format: " + hrToString(hr));
        return false;
    }

    // For loopback, we must use shared mode with the device's mix format.
    // For capture devices, we also use shared mode for simplicity.
    DWORD streamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (isLoopback_) {
        streamFlags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
    }

    // Use 20ms buffer for low latency
    REFERENCE_TIME bufferDuration = 200000; // 20ms in 100ns units

    hr = audioClient_->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        streamFlags,
        bufferDuration,
        0,  // periodicity (must be 0 for shared mode)
        mixFormat_,
        nullptr);

    if (FAILED(hr)) {
        reportError("IAudioClient::Initialize failed: " + hrToString(hr));
        return false;
    }

    // Set event handle for event-driven capture
    hr = audioClient_->SetEventHandle(eventHandle_);
    if (FAILED(hr)) {
        reportError("SetEventHandle failed: " + hrToString(hr));
        return false;
    }

    // Get capture client
    hr = audioClient_->GetService(__uuidof(IAudioCaptureClient),
                                  reinterpret_cast<void**>(&captureClient_));
    if (FAILED(hr) || !captureClient_) {
        reportError("Failed to get IAudioCaptureClient: " + hrToString(hr));
        return false;
    }

    // Store actual format parameters
    channelCount_ = mixFormat_->nChannels;
    sampleRate_ = mixFormat_->nSamplesPerSec;

    // We always convert to 16-bit integer PCM in captureThread,
    // so report 16 regardless of the device's native format.
    bitsPerSample_ = 16;

    return true;
}

// ============================================================================
// start
// ============================================================================
bool WasapiCapture::start() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_ || capturing_) return false;

    ResetEvent(stopEvent_);

    HRESULT hr = audioClient_->Start();
    if (FAILED(hr)) {
        reportError("IAudioClient::Start failed: " + hrToString(hr));
        return false;
    }

    // Record the start time for timestamp calculation
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    startQpc_ = qpc.QuadPart;

    capturing_ = true;
    captureThread_ = std::thread(&WasapiCapture::captureThread, this);

    return true;
}

// ============================================================================
// stop
// ============================================================================
bool WasapiCapture::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!capturing_) return true;

        capturing_ = false;
        SetEvent(stopEvent_);
    }

    if (captureThread_.joinable()) {
        captureThread_.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (audioClient_) {
        audioClient_->Stop();
        audioClient_->Reset();
    }

    return true;
}

// ============================================================================
// captureThread
// ============================================================================
void WasapiCapture::captureThread() {
    ScopedCom com;

    HANDLE waitHandles[2] = { eventHandle_, stopEvent_ };

    while (capturing_) {
        DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, 2000);

        if (waitResult == WAIT_OBJECT_0 + 1) {
            // Stop event signaled
            break;
        }

        if (waitResult == WAIT_TIMEOUT) {
            // Timeout - if still capturing, just loop
            if (!capturing_) break;
            continue;
        }

        if (waitResult == WAIT_FAILED) {
            reportError("WaitForMultipleObjects failed: " + std::to_string(GetLastError()));
            break;
        }

        // Buffer event signaled - read all available packets
        UINT32 packetLength = 0;
        HRESULT hr = captureClient_->GetNextPacketSize(&packetLength);

        if (FAILED(hr)) {
            // Device may have been disconnected
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                reportError("Audio device disconnected");
            } else {
                reportError("GetNextPacketSize failed: " + hrToString(hr));
            }
            break;
        }

        while (packetLength > 0 && capturing_) {
            BYTE* data = nullptr;
            UINT32 numFramesAvailable = 0;
            DWORD flags = 0;
            UINT64 devicePosition = 0;
            UINT64 qpcPosition = 0;

            hr = captureClient_->GetBuffer(&data, &numFramesAvailable, &flags,
                                           &devicePosition, &qpcPosition);
            if (FAILED(hr)) {
                if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                    reportError("Audio device disconnected during capture");
                } else {
                    reportError("GetBuffer failed: " + hrToString(hr));
                }
                capturing_ = false;
                break;
            }

            if (numFramesAvailable > 0) {
                // Calculate timestamp in 100ns units
                int64_t timestamp = 0;
                if (qpcPosition > 0) {
                    // qpcPosition is in QPC units; convert to 100ns
                    timestamp = static_cast<int64_t>(
                        (static_cast<double>(qpcPosition - startQpc_) / qpcFrequency_) * 10000000.0);
                } else {
                    LARGE_INTEGER now;
                    QueryPerformanceCounter(&now);
                    timestamp = static_cast<int64_t>(
                        (static_cast<double>(now.QuadPart - startQpc_) / qpcFrequency_) * 10000000.0);
                }
                if (timestamp < 0) timestamp = 0;

                // Calculate data size
                UINT32 bytesPerFrame = mixFormat_->nBlockAlign;
                UINT32 dataSize = numFramesAvailable * bytesPerFrame;

                // Build AudioBuffer
                AudioBuffer buffer;
                buffer.sampleCount = numFramesAvailable;
                buffer.channelCount = channelCount_;
                buffer.sampleRate = sampleRate_;
                buffer.timestamp = timestamp;

                bool isSilence = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;

                // Always output 16-bit integer PCM for compatibility
                // with Media Foundation encoders (AAC, MP3, WMA)
                int outBps = 16;
                buffer.bitsPerSample = outBps;

                uint32_t outBytesPerSample = outBps / 8;
                uint32_t outSize = numFramesAvailable * channelCount_ * outBytesPerSample;
                buffer.data.resize(outSize);

                if (isSilence) {
                    // Fill with silence
                    std::memset(buffer.data.data(), 0, outSize);
                } else {
                    // Determine source format
                    bool srcIsFloat = false;
                    int srcBps = mixFormat_->wBitsPerSample;
                    if (mixFormat_->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
                        WAVEFORMATEXTENSIBLE* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mixFormat_);
                        static const GUID KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_LOCAL =
                            {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
                        srcIsFloat = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_LOCAL);
                    } else if (mixFormat_->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
                        srcIsFloat = true;
                    }

                    uint32_t totalSamples = numFramesAvailable * channelCount_;

                    if (srcIsFloat && srcBps == 32 && outBps == 16) {
                        // Float32 -> Int16
                        const float* src = reinterpret_cast<const float*>(data);
                        int16_t* dst = reinterpret_cast<int16_t*>(buffer.data.data());
                        for (uint32_t i = 0; i < totalSamples; ++i) {
                            float sample = src[i];
                            // Clamp to [-1, 1]
                            if (sample > 1.0f) sample = 1.0f;
                            if (sample < -1.0f) sample = -1.0f;
                            dst[i] = static_cast<int16_t>(sample * 32767.0f);
                        }
                    } else if (srcIsFloat && srcBps == 32 && outBps == 32) {
                        // Float32 pass-through
                        std::memcpy(buffer.data.data(), data, outSize);
                    } else if (!srcIsFloat && srcBps == 16 && outBps == 16) {
                        // Int16 pass-through
                        std::memcpy(buffer.data.data(), data, outSize);
                    } else if (!srcIsFloat && srcBps == 16 && outBps == 32) {
                        // Int16 -> Float32
                        const int16_t* src = reinterpret_cast<const int16_t*>(data);
                        float* dst = reinterpret_cast<float*>(buffer.data.data());
                        for (uint32_t i = 0; i < totalSamples; ++i) {
                            dst[i] = static_cast<float>(src[i]) / 32768.0f;
                        }
                    } else {
                        // Fallback: raw copy
                        uint32_t copySize = std::min(dataSize, outSize);
                        std::memcpy(buffer.data.data(), data, copySize);
                        if (copySize < outSize) {
                            std::memset(buffer.data.data() + copySize, 0, outSize - copySize);
                        }
                    }
                }

                // Deliver to callback
                AudioCallback cb;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    cb = audioCallback_;
                }
                if (cb) {
                    cb(buffer);
                }
            }

            captureClient_->ReleaseBuffer(numFramesAvailable);

            hr = captureClient_->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) {
                if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                    reportError("Audio device disconnected");
                }
                capturing_ = false;
                break;
            }
        }
    }
}

// ============================================================================
// Setters / Getters
// ============================================================================
void WasapiCapture::setAudioCallback(AudioCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    audioCallback_ = std::move(callback);
}

void WasapiCapture::setErrorCallback(ErrorCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    errorCallback_ = std::move(callback);
}

int WasapiCapture::getChannelCount() const { return channelCount_; }
int WasapiCapture::getSampleRate() const { return sampleRate_; }
int WasapiCapture::getBitsPerSample() const { return bitsPerSample_; }
bool WasapiCapture::isCapturing() const { return capturing_; }

// ============================================================================
// releaseResources
// ============================================================================
void WasapiCapture::releaseResources() {
    safeRelease(captureClient_);
    safeRelease(audioClient_);
    safeRelease(device_);

    if (mixFormat_) {
        CoTaskMemFree(mixFormat_);
        mixFormat_ = nullptr;
    }

    if (eventHandle_) {
        CloseHandle(eventHandle_);
        eventHandle_ = nullptr;
    }

    if (stopEvent_) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }

    initialized_ = false;
}

// ============================================================================
// reportError
// ============================================================================
void WasapiCapture::reportError(const std::string& msg) {
    ErrorCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = errorCallback_;
    }
    if (cb) {
        cb(msg);
    }
}

} // namespace pb
