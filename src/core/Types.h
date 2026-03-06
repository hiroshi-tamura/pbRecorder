#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <chrono>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace pb {

// Video codec
enum class VideoCodec {
    H264,
    WMV
};

// Audio codec
enum class AudioCodec {
    AAC,
    MP3,
    Opus,
    Vorbis,
    PCM,
    WMA
};

// Container format
enum class ContainerFormat {
    MP4,
    MKV,
    WMV
};

// Capture mode
enum class CaptureMode {
    Screen,    // Full display
    Window,    // Specific window
    Region     // User-selected region
};

// Audio device type
enum class AudioDeviceType {
    WASAPI_Render,    // System audio (loopback)
    WASAPI_Capture,   // Microphone
    ASIO
};

struct MonitorInfo {
    std::wstring name;
    std::wstring deviceName;
    int x, y, width, height;
    float dpiScaleX, dpiScaleY;
    HMONITOR hMonitor;
    int index;
};

struct WindowInfo {
    HWND hwnd;
    std::wstring title;
    std::wstring className;
    std::wstring processName;
    DWORD processId;
    RECT rect;
};

struct AudioDeviceInfo {
    std::wstring id;
    std::wstring name;
    AudioDeviceType type;
    int channelCount;
    int sampleRate;
    int bitsPerSample;
    // ASIO channel range (0-based)
    int asioStartChannel = 0;
    int asioEndChannel = -1; // -1 = use all channels
};

struct RegionRect {
    int x, y, width, height;
};

struct VideoSettings {
    VideoCodec codec = VideoCodec::H264;
    int width = 0;          // 0 = auto (from capture source)
    int height = 0;
    int fps = 60;
    int bitrate = 8000000;  // 8 Mbps default
    int quality = 70;       // 0-100, used for VBR
    bool useHardwareEncoder = true;
    bool realtimeEncode = true;  // true=normal, false=fast preset for less CPU
};

struct AudioSettings {
    AudioCodec codec = AudioCodec::AAC;
    int sampleRate = 48000;
    int channelCount = 2;   // 0 = auto (from device)
    int bitrate = 192000;   // 192 kbps default
    int bitsPerSample = 16;
    int quality = 70;       // 0-100
};

struct CaptureConfig {
    CaptureMode mode = CaptureMode::Screen;
    int monitorIndex = 0;
    HWND targetWindow = nullptr;
    RegionRect region = {};
    bool captureCursor = true;
};

struct RecordingConfig {
    CaptureConfig capture;
    VideoSettings video;
    AudioSettings audio;
    ContainerFormat container = ContainerFormat::MP4;
    std::wstring outputPath;
    bool recordAudio = true;
    AudioDeviceInfo audioDevice;       // backward compat (primary device)
    AudioDeviceInfo outputAudioDevice; // system audio (loopback)
    AudioDeviceInfo inputAudioDevice;  // microphone
    bool useOutputAudio = false;       // record system audio
    bool useInputAudio = false;        // record microphone
};

// Video frame from capture
struct VideoFrame {
    ComPtr<ID3D11Texture2D> texture;
    int64_t timestamp;  // 100ns units
    uint32_t width;
    uint32_t height;
};

// Audio buffer from capture
struct AudioBuffer {
    std::vector<uint8_t> data;
    int64_t timestamp;   // 100ns units
    uint32_t sampleCount;
    uint32_t channelCount;
    uint32_t sampleRate;
    uint32_t bitsPerSample;
};

// Callbacks
using FrameCallback = std::function<void(const VideoFrame&)>;
using AudioCallback = std::function<void(const AudioBuffer&)>;
using ErrorCallback = std::function<void(const std::string&)>;

// Thread-safe queue
template<typename T>
class ThreadSafeQueue {
public:
    void push(T item) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(std::move(item));
        }
        cv_.notify_one();
    }

    bool tryPop(T& item, std::chrono::milliseconds timeout = std::chrono::milliseconds(100)) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, timeout, [this] { return !queue_.empty() || stopped_; }))
            return false;
        if (stopped_ && queue_.empty())
            return false;
        item = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
        }
        cv_.notify_all();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!queue_.empty()) queue_.pop();
        stopped_ = false;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    std::queue<T> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool stopped_ = false;
};

// Utility: HRESULT to string
inline std::string hrToString(HRESULT hr) {
    char buf[64];
    snprintf(buf, sizeof(buf), "HRESULT 0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

#define PB_CHECK_HR(hr, msg) \
    do { \
        HRESULT _hr = (hr); \
        if (FAILED(_hr)) { \
            throw std::runtime_error(std::string(msg) + ": " + hrToString(_hr)); \
        } \
    } while(0)

} // namespace pb
