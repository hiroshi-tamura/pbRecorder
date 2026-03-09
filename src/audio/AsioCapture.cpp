#include "AsioCapture.h"

#include <cstring>
#include <cmath>
#include <algorithm>
#include <stdexcept>

#include <windows.h>

namespace pb {

#ifdef ASIO_AVAILABLE

// ============================================================================
// Singleton instance for ASIO callbacks (ASIO SDK uses C-style callbacks)
// ============================================================================
AsioCapture* AsioCapture::instance_ = nullptr;

// ============================================================================
// Constructor / Destructor
// ============================================================================
AsioCapture::AsioCapture() {
    instance_ = this;
}

AsioCapture::~AsioCapture() {
    stop();
    releaseResources();
    if (instance_ == this) instance_ = nullptr;
}

// ============================================================================
// enumerateDevices - scans registry for ASIO drivers
// ============================================================================
std::vector<AudioDeviceInfo> AsioCapture::enumerateDevices() {
    std::vector<AudioDeviceInfo> result;

    HKEY asioKey = nullptr;
    LONG regResult = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                                   L"SOFTWARE\\ASIO",
                                   0, KEY_READ, &asioKey);
    if (regResult != ERROR_SUCCESS) {
        // Try WOW64 32-bit registry on 64-bit Windows
        regResult = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                                  L"SOFTWARE\\ASIO",
                                  0, KEY_READ | KEY_WOW64_32KEY, &asioKey);
        if (regResult != ERROR_SUCCESS) return result;
    }

    DWORD subKeyCount = 0;
    RegQueryInfoKeyW(asioKey, nullptr, nullptr, nullptr, &subKeyCount,
                     nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);

    for (DWORD i = 0; i < subKeyCount; ++i) {
        wchar_t subKeyName[256] = {};
        DWORD nameLen = 256;
        if (RegEnumKeyExW(asioKey, i, subKeyName, &nameLen,
                          nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
            continue;
        }

        HKEY driverKey = nullptr;
        if (RegOpenKeyExW(asioKey, subKeyName, 0, KEY_READ, &driverKey) != ERROR_SUCCESS) {
            continue;
        }

        // Read CLSID
        wchar_t clsidStr[128] = {};
        DWORD clsidSize = sizeof(clsidStr);
        DWORD type = REG_SZ;
        if (RegQueryValueExW(driverKey, L"CLSID", nullptr, &type,
                             reinterpret_cast<LPBYTE>(clsidStr), &clsidSize) == ERROR_SUCCESS) {

            // Read description
            wchar_t description[256] = {};
            DWORD descSize = sizeof(description);
            if (RegQueryValueExW(driverKey, L"Description", nullptr, &type,
                                 reinterpret_cast<LPBYTE>(description), &descSize) != ERROR_SUCCESS) {
                wcscpy(description, subKeyName);
            }

            // Add as input device (recording from interface inputs)
            AudioDeviceInfo inputInfo;
            inputInfo.id = clsidStr;
            inputInfo.name = std::wstring(description) + L" (In)";
            inputInfo.type = AudioDeviceType::ASIO;
            inputInfo.channelCount = 2;
            inputInfo.sampleRate = 48000;
            inputInfo.bitsPerSample = 32;
            result.push_back(std::move(inputInfo));

            // Add as output device (recording what the interface plays back)
            AudioDeviceInfo outputInfo;
            outputInfo.id = clsidStr;
            outputInfo.name = std::wstring(description) + L" (Out)";
            outputInfo.type = AudioDeviceType::ASIO_Output;
            outputInfo.channelCount = 2;
            outputInfo.sampleRate = 48000;
            outputInfo.bitsPerSample = 32;
            result.push_back(std::move(outputInfo));
        }

        RegCloseKey(driverKey);
    }

    RegCloseKey(asioKey);
    return result;
}

// ============================================================================
// initialize - uses AsioDrivers to load driver, then global ASIOxxx() API
// ============================================================================
bool AsioCapture::initialize(const AudioDeviceInfo& device) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (capturing_) return false;
    releaseResources();

    deviceInfo_ = device;
    isOutputCapture_ = (device.type == AudioDeviceType::ASIO_Output);

    // Use AsioDrivers to load the driver by name
    // Strip " (In)" / " (Out)" suffix added by enumerateDevices
    asioDrivers_ = new AsioDrivers();
    std::wstring wname = device.name;
    if (wname.size() > 5 && (wname.substr(wname.size() - 5) == L" (In)" ||
                              wname.substr(wname.size() - 6) == L" (Out)")) {
        size_t pos = wname.rfind(L" (");
        if (pos != std::wstring::npos) wname = wname.substr(0, pos);
    }
    std::string driverName(wname.begin(), wname.end());
    if (!asioDrivers_->loadDriver(const_cast<char*>(driverName.c_str()))) {
        reportError("Failed to load ASIO driver: " + driverName);
        delete asioDrivers_;
        asioDrivers_ = nullptr;
        return false;
    }

    // Initialize the ASIO driver via global API
    memset(&driverInfo_, 0, sizeof(driverInfo_));
    driverInfo_.asioVersion = 2;
    driverInfo_.sysRef = GetDesktopWindow();

    if (ASIOInit(&driverInfo_) != ASE_OK) {
        reportError("ASIO driver init failed: " + std::string(driverInfo_.errorMessage));
        asioDrivers_->removeCurrentDriver();
        delete asioDrivers_;
        asioDrivers_ = nullptr;
        return false;
    }

    // Query channels
    if (ASIOGetChannels(&inputChannels_, &outputChannels_) != ASE_OK) {
        reportError("Failed to get ASIO channel count");
        return false;
    }

    long totalChannels = isOutputCapture_ ? outputChannels_ : inputChannels_;
    if (totalChannels <= 0) {
        reportError(isOutputCapture_ ? "ASIO driver has no output channels"
                                     : "ASIO driver has no input channels");
        return false;
    }

    // Determine channel range
    long startCh = device.asioStartChannel;
    long endCh = device.asioEndChannel;
    if (endCh < 0 || endCh >= totalChannels) endCh = totalChannels - 1;
    if (startCh < 0) startCh = 0;
    if (startCh > endCh) startCh = endCh;
    channelCount_ = static_cast<int>(endCh - startCh + 1);

    // Query sample rate
    ASIOSampleRate currentRate;
    if (ASIOGetSampleRate(&currentRate) == ASE_OK) {
        sampleRate_ = static_cast<int>(currentRate);
    } else {
        sampleRate_ = 48000;
        ASIOSetSampleRate(48000.0);
    }

    // Query buffer sizes
    if (ASIOGetBufferSize(&minBufferSize_, &maxBufferSize_,
                          &preferredBufferSize_, &bufferGranularity_) != ASE_OK) {
        reportError("Failed to get ASIO buffer sizes");
        return false;
    }

    // Query channel info to determine sample type
    ASIOBool isInput = isOutputCapture_ ? ASIOFalse : ASIOTrue;
    channelInfos_ = new ASIOChannelInfo[totalChannels];
    for (long ch = 0; ch < totalChannels; ++ch) {
        channelInfos_[ch].channel = ch;
        channelInfos_[ch].isInput = isInput;
        if (ASIOGetChannelInfo(&channelInfos_[ch]) != ASE_OK) {
            reportError("Failed to get ASIO channel info");
            return false;
        }
    }
    sampleType_ = channelInfos_[0].type;

    // Determine bits per sample from ASIO sample type
    switch (sampleType_) {
        case ASIOSTInt16LSB: bitsPerSample_ = 16; break;
        case ASIOSTInt24LSB: bitsPerSample_ = 24; break;
        case ASIOSTInt32LSB: bitsPerSample_ = 32; break;
        case ASIOSTFloat32LSB: bitsPerSample_ = 32; break;
        case ASIOSTFloat64LSB: bitsPerSample_ = 64; break;
        default: bitsPerSample_ = 32; break;
    }

    // Allocate buffer infos for the channels we're capturing
    bufferInfos_ = new ASIOBufferInfo[totalChannels];
    for (long ch = 0; ch < totalChannels; ++ch) {
        bufferInfos_[ch].isInput = isInput;
        bufferInfos_[ch].channelNum = ch;
        bufferInfos_[ch].buffers[0] = nullptr;
        bufferInfos_[ch].buffers[1] = nullptr;
    }

    // Set up callbacks
    asioCallbacks_.bufferSwitch = &AsioCapture::bufferSwitchCallback;
    asioCallbacks_.sampleRateDidChange = &AsioCapture::sampleRateDidChangeCallback;
    asioCallbacks_.asioMessage = &AsioCapture::asioMessageCallback;
    asioCallbacks_.bufferSwitchTimeInfo = &AsioCapture::bufferSwitchTimeInfoCallback;

    // Create buffers
    activeChannels_ = totalChannels;
    if (ASIOCreateBuffers(bufferInfos_, totalChannels,
                          preferredBufferSize_, &asioCallbacks_) != ASE_OK) {
        reportError("Failed to create ASIO buffers");
        return false;
    }

    initialized_ = true;
    return true;
}

// ============================================================================
// start
// ============================================================================
bool AsioCapture::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || capturing_) return false;

    if (ASIOStart() != ASE_OK) {
        reportError("ASIOStart failed");
        return false;
    }

    capturing_ = true;
    return true;
}

// ============================================================================
// stop
// ============================================================================
bool AsioCapture::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!capturing_) return true;

    ASIOStop();
    capturing_ = false;
    return true;
}

// ============================================================================
// ASIO Callback trampolines
// ============================================================================
void AsioCapture::bufferSwitchCallback(long index, ASIOBool processNow) {
    if (instance_) instance_->onBufferSwitch(index, processNow);
}

void AsioCapture::sampleRateDidChangeCallback(ASIOSampleRate sRate) {
    if (instance_) {
        instance_->sampleRate_ = static_cast<int>(static_cast<double>(sRate));
    }
}

long AsioCapture::asioMessageCallback(long selector, long value, void* message, double* opt) {
    switch (selector) {
        case kAsioSelectorSupported:
            switch (value) {
                case kAsioResetRequest:
                case kAsioEngineVersion:
                case kAsioResyncRequest:
                case kAsioLatenciesChanged:
                case kAsioSupportsTimeInfo:
                case kAsioSupportsTimeCode:
                    return 1;
            }
            return 0;
        case kAsioEngineVersion:
            return 2;
        case kAsioResetRequest:
            return 1;
        case kAsioResyncRequest:
            return 1;
        case kAsioLatenciesChanged:
            return 1;
        case kAsioSupportsTimeInfo:
            return 1;
        case kAsioSupportsTimeCode:
            return 0;
        default:
            return 0;
    }
}

ASIOTime* AsioCapture::bufferSwitchTimeInfoCallback(ASIOTime* timeInfo, long index, ASIOBool processNow) {
    if (instance_) instance_->onBufferSwitch(index, processNow);
    return timeInfo;
}

// ============================================================================
// onBufferSwitch - convert ASIO non-interleaved to interleaved PCM
// ============================================================================
void AsioCapture::onBufferSwitch(long index, ASIOBool /*processNow*/) {
    if (!capturing_ || !bufferInfos_) return;

    convertAndDeliver(index);
}

void AsioCapture::convertAndDeliver(long bufferIndex) {
    long frames = preferredBufferSize_;

    // Determine channel range
    long startCh = deviceInfo_.asioStartChannel;
    long endCh = deviceInfo_.asioEndChannel;
    if (endCh < 0 || endCh >= activeChannels_) endCh = activeChannels_ - 1;
    if (startCh < 0) startCh = 0;
    if (startCh > endCh) startCh = endCh;
    int outChannels = static_cast<int>(endCh - startCh + 1);

    // Output: interleaved 16-bit PCM
    int outBps = 16;
    uint32_t outBytesPerSample = outBps / 8;
    uint32_t outSize = frames * outChannels * outBytesPerSample;

    AudioBuffer audioBuffer;
    audioBuffer.data.resize(outSize);
    audioBuffer.sampleCount = static_cast<uint32_t>(frames);
    audioBuffer.channelCount = static_cast<uint32_t>(outChannels);
    audioBuffer.sampleRate = static_cast<uint32_t>(sampleRate_);
    audioBuffer.bitsPerSample = outBps;

    // Timestamp from QPC
    LARGE_INTEGER qpc, freq;
    QueryPerformanceCounter(&qpc);
    QueryPerformanceFrequency(&freq);
    audioBuffer.timestamp = static_cast<int64_t>(
        (static_cast<double>(qpc.QuadPart) / freq.QuadPart) * 10000000.0);

    int16_t* dst = reinterpret_cast<int16_t*>(audioBuffer.data.data());

    for (long frame = 0; frame < frames; ++frame) {
        for (long ch = startCh; ch <= endCh; ++ch) {
            void* srcBuf = bufferInfos_[ch].buffers[bufferIndex];
            int16_t sample = 0;

            switch (sampleType_) {
                case ASIOSTInt16LSB: {
                    const int16_t* src = static_cast<const int16_t*>(srcBuf);
                    sample = src[frame];
                    break;
                }
                case ASIOSTInt24LSB: {
                    const uint8_t* src = static_cast<const uint8_t*>(srcBuf);
                    const uint8_t* p = src + frame * 3;
                    // 24-bit little-endian: use upper 16 bits
                    int32_t val = static_cast<int32_t>(
                        (static_cast<uint32_t>(p[2]) << 24) |
                        (static_cast<uint32_t>(p[1]) << 16) |
                        (static_cast<uint32_t>(p[0]) << 8));
                    val >>= 16; // sign-extended shift
                    sample = static_cast<int16_t>(val);
                    break;
                }
                case ASIOSTInt32LSB: {
                    const int32_t* src = static_cast<const int32_t*>(srcBuf);
                    // Shift down to 16-bit
                    sample = static_cast<int16_t>(src[frame] >> 16);
                    break;
                }
                case ASIOSTFloat32LSB: {
                    const float* src = static_cast<const float*>(srcBuf);
                    float val = src[frame];
                    if (val > 1.0f) val = 1.0f;
                    if (val < -1.0f) val = -1.0f;
                    sample = static_cast<int16_t>(val * 32767.0f);
                    break;
                }
                case ASIOSTFloat64LSB: {
                    const double* src = static_cast<const double*>(srcBuf);
                    double val = src[frame];
                    if (val > 1.0) val = 1.0;
                    if (val < -1.0) val = -1.0;
                    sample = static_cast<int16_t>(val * 32767.0);
                    break;
                }
                default:
                    sample = 0;
                    break;
            }

            dst[frame * outChannels + (ch - startCh)] = sample;
        }
    }

    // Deliver
    AudioCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = audioCallback_;
    }
    if (cb) {
        cb(audioBuffer);
    }
}

// ============================================================================
// releaseResources
// ============================================================================
void AsioCapture::releaseResources() {
    if (initialized_) {
        ASIODisposeBuffers();
        ASIOExit();
    }

    if (asioDrivers_) {
        asioDrivers_->removeCurrentDriver();
        delete asioDrivers_;
        asioDrivers_ = nullptr;
    }

    delete[] bufferInfos_;
    bufferInfos_ = nullptr;
    delete[] channelInfos_;
    channelInfos_ = nullptr;

    initialized_ = false;
}

// ============================================================================
// Setters / Getters
// ============================================================================
void AsioCapture::setAudioCallback(AudioCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    audioCallback_ = std::move(callback);
}

void AsioCapture::setErrorCallback(ErrorCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    errorCallback_ = std::move(callback);
}

int AsioCapture::getChannelCount() const { return channelCount_; }
int AsioCapture::getSampleRate() const { return sampleRate_; }
int AsioCapture::getBitsPerSample() const { return bitsPerSample_; }
bool AsioCapture::isCapturing() const { return capturing_; }

void AsioCapture::reportError(const std::string& msg) {
    ErrorCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = errorCallback_;
    }
    if (cb) cb(msg);
}

#else // !ASIO_AVAILABLE

// ============================================================================
// Stub implementation when ASIO SDK is not available
// ============================================================================

AsioCapture::AsioCapture() {}
AsioCapture::~AsioCapture() {}

std::vector<AudioDeviceInfo> AsioCapture::enumerateDevices() {
    return {};
}

bool AsioCapture::initialize(const AudioDeviceInfo& /*device*/) {
    reportError("ASIO support is not available (ASIO SDK not compiled in)");
    return false;
}

bool AsioCapture::start() { return false; }

bool AsioCapture::stop() { return true; }

void AsioCapture::setAudioCallback(AudioCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    audioCallback_ = std::move(callback);
}

void AsioCapture::setErrorCallback(ErrorCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    errorCallback_ = std::move(callback);
}

int AsioCapture::getChannelCount() const { return 0; }
int AsioCapture::getSampleRate() const { return 0; }
int AsioCapture::getBitsPerSample() const { return 0; }
bool AsioCapture::isCapturing() const { return false; }

void AsioCapture::reportError(const std::string& msg) {
    ErrorCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = errorCallback_;
    }
    if (cb) cb(msg);
}

void AsioCapture::releaseResources() {}

#endif // ASIO_AVAILABLE

} // namespace pb
