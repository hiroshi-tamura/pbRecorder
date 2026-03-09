# pbRecorder

A feature-rich screen recorder for Windows. Uses DXGI Desktop Duplication for high-performance capture with support for multiple codecs and container formats.

[日本語版 README](README_ja.md)

## Features

### Capture Modes
- **Full Screen** — Record entire display (multi-monitor support)
- **Window** — Record a specific window
- **Region** — Record an arbitrary rectangular area
  - **Auto-adjust** — Automatically snaps to nearby window edges
  - Edges can be fine-tuned by dragging after selection

### Video
- **Codecs**: H.264, WMV
- **Hardware encoding**: GPU encoding via Media Foundation
- **Frame rate**: Up to 240fps
- **Bitrate**: Configurable (default 8 Mbps)
- **Mouse cursor**: Toggle capture on/off

### Audio
- **WASAPI**: System audio (loopback) and microphone input simultaneously
- **ASIO**: Low-latency ASIO device support (requires ASIO SDK)
- **Codecs**: AAC, MP3, Opus, Vorbis, PCM, WMA

### Container Formats
- **MP4** (.mp4) — H.264 + AAC/MP3
- **MKV** (.mkv) — H.264 + Opus/Vorbis/PCM (native implementation via libmatroska)
- **WMV** (.wmv) — WMV + WMA

### Other
- **Preset system** — Save and load recording configurations
- **Language switching** — English / Japanese UI
- **Portable** — Settings stored in JSON file next to exe (no registry)

## System Requirements

- Windows 10 or later (64-bit)
- DirectX 11 compatible GPU
- Qt 6 runtime (included in release)

## Installation

1. Download ZIP from [Releases](https://github.com/hiroshi-tamura/pbRecorder/releases)
2. Extract to any folder
3. Run `pbRecorder.exe`

## Building from Source

### Prerequisites
- CMake 3.24+
- Qt 6.9+
- MinGW-w64 or MSVC
- (Optional) ASIO SDK — place in `third_party/asiosdk/`

### Build Steps

```bash
mkdir build && cd build
cmake .. -G "MinGW Makefiles" -DCMAKE_PREFIX_PATH="<path-to-Qt6>"
cmake --build . --config Release -- -j4
```

Third-party libraries (libebml, libmatroska, libogg, libvorbis, libopus) are automatically downloaded and built via CMake FetchContent.

## Usage

1. Select capture source (Full Screen / Window / Region)
2. Select audio devices if needed
3. Configure container format and codecs
4. Set output folder and filename
5. Click Record (or `Ctrl+R`) to start/stop

### Keyboard Shortcuts
- `Ctrl+R` — Start/stop recording
- Region selection: `Enter` to confirm, `Esc` to cancel

## Tech Stack

- **UI**: Qt 6 (Widgets)
- **Capture**: DXGI Desktop Duplication API
- **Encoding**: Media Foundation (H.264/AAC/MP3/WMV/WMA)
- **MKV container**: libmatroska + libebml (native implementation)
- **Audio capture**: WASAPI (loopback/mic), ASIO
- **Audio codecs**: libopus, libvorbis (for MKV)

## Encoding & Licensing

pbRecorder uses a **patent and license-clean architecture**.

### MP4 (H.264 + AAC/MP3)
- Encoded using Windows built-in **Media Foundation**
- No codec libraries are bundled — uses OS-provided H.264/AAC encoders
- **No FFmpeg, x264, or other GPL/LGPL codec libraries**

### MKV (H.264 + Opus/Vorbis/PCM)
- Video: Media Foundation generates raw H.264 NALUs, written directly to MKV via **libmatroska/libebml**
- Audio: **libopus** (BSD) or **libvorbis** (BSD)
- Container: **libmatroska** (LGPL) + **libebml** (LGPL)
- PCM (uncompressed) recording also available

### WMV (WMV + WMA)
- Encoded using Media Foundation (OS built-in)

### Libraries and Licenses

| Library | Version | License | Purpose |
|---------|---------|---------|---------|
| Qt 6 | 6.9+ | LGPL v3 | UI framework |
| libmatroska | 1.7.1 | LGPL v2.1 | MKV container writing |
| libebml | 1.4.5 | LGPL v2.1 | EBML (MKV foundation) |
| libopus | 1.4 | BSD 3-Clause | Opus audio encoding |
| libvorbis | 1.3.7 | BSD 3-Clause | Vorbis audio encoding |
| libogg | 1.3.5 | BSD 3-Clause | Ogg foundation library |
| Media Foundation | OS built-in | Windows standard | H.264/AAC/MP3/WMV/WMA encoding |
| DXGI | OS built-in | Windows standard | Screen capture |
| WASAPI | OS built-in | Windows standard | Audio capture |

- **No GPL contamination**: No GPL/AGPL libraries are used
- **No FFmpeg**: Codec processing does not use FFmpeg
- All third-party libraries are BSD or LGPL, suitable for commercial use

## License

MIT License
