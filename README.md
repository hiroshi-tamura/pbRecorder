<p align="center">
  <img src="resources/icon.png" alt="pbRecorder" width="128">
</p>

# pbRecorder

A feature-rich screen recorder for Windows. Uses DXGI Desktop Duplication for high-performance capture with support for multiple codecs and container formats. Includes both GUI and CLI.

[日本語版 README](README_ja.md)

## Features

### Capture Modes
- **Full Screen** — Record entire display (multi-monitor support)
- **Window** — Record a specific window
- **Region** — Record an arbitrary rectangular area
  - **Auto-adjust** — Automatically snaps to nearby window edges
  - Edges can be fine-tuned by dragging after selection
- **UI Region Tracking** — Select a UI Automation element under the mouse and keep the recording crop aligned to that element while it moves
  - `Ctrl+Space`, `Enter`, or click confirms the highlighted UI region; `Esc` cancels
  - **Crop from parent window** is enabled by default to reduce unrelated overlapping windows in the captured image
  - Falls back to screen cropping when the target application cannot be captured through `PrintWindow`

### Video
- **Codecs**: H.264, WMV
- **Hardware encoding**: GPU encoding via Media Foundation
- **Realtime H.264 MP4**: Uses Media Foundation fragmented MP4 output to keep recording data committed during capture and reduce stop-time finalization work
- **H.264 options**: Profile (Baseline/Main/High), Level (Auto/4.0–5.1)
- **Frame rate**: Up to 240fps
- **Bitrate**: Configurable (default 8 Mbps)
- **Mouse cursor**: Toggle capture on/off

### Audio
- **WASAPI**: System audio (loopback) or microphone input
- **ASIO**: Optional low-latency ASIO device support (requires a separately obtained ASIO SDK)
- **Codecs**: AAC, MP3, Opus, Vorbis, PCM, WMA

### Container Formats
- **MP4** (.mp4) — H.264 + AAC/MP3
- **MKV** (.mkv) — H.264 + AAC/Opus/Vorbis/PCM (native implementation via libmatroska)
- **WMV** (.wmv) — WMV + WMA

### Other
- **Preset system** — Save and load recording configurations
- **Language switching** — English / Japanese UI
- **Portable** — Settings stored in JSON file next to exe (no registry)
- **CLI support** — Full-featured command-line interface (`pbRecorder-cli.exe`)

## System Requirements

- Windows 10 or later (64-bit)
- DirectX 11 compatible GPU
- Qt 6 runtime (included in release)

## Installation

1. Download ZIP from [Releases](https://github.com/hiroshi-tamura/pbRecorder/releases)
2. Extract to any folder
3. Run `pbRecorder.exe` (GUI) or `pbRecorder-cli.exe` (CLI)

## GUI Usage

1. Select capture source (Full Screen / Window / Region / UI Region Tracking)
2. Select audio devices if needed
3. Configure container format and codecs
4. Set output folder and filename
5. Click Record (or `Ctrl+R`) to start/stop

For UI Region Tracking, choose **UI Region Tracking**, click **Select**, hover the target pane/control, then confirm the highlighted region. Use **Crop from parent window** when you want to avoid unrelated overlapping windows in the capture.

### Keyboard Shortcuts
- `Ctrl+R` — Start/stop recording from the main window
- `Ctrl+Shift+R` — Global start/stop hotkey
- Region selection: `Enter` to confirm, `Esc` to cancel
- UI region selection: `Ctrl+Space`, `Enter`, or click to confirm; `Esc` to cancel; mouse wheel / arrow keys cycle nearby parent UI regions

### UI Region Tracking Notes

UI Region Tracking is available in the GUI only. It uses Windows UI Automation to identify the selected pane/control and records the corresponding rectangle. The default **Crop from parent window** option captures the parent window first and then crops the selected UI region, which can avoid unrelated windows covering the target. Some applications do not render correctly through `PrintWindow`; disable the option to record the on-screen composed pixels instead.

Limitations: UI tracking depends on the target application's UI Automation tree and window rendering behavior. Custom-rendered, elevated, hidden, or minimized applications may not expose stable UI regions. Parent-window crop may produce black or stale frames in applications that do not support `PrintWindow` well.

## CLI Usage

`pbRecorder-cli.exe` provides full recording functionality from the command line.
UI Region Tracking is GUI-only because it requires interactive UI element selection.

### Device Enumeration

```bash
pbRecorder-cli --list-monitors
pbRecorder-cli --list-windows
pbRecorder-cli --list-audio-devices
```

### Basic Recording

```bash
# Record full screen, stop with Ctrl+C
pbRecorder-cli --cli --auto-name -o ./Output/

# Record for 60 seconds
pbRecorder-cli --cli --duration 60 --auto-name -o ./Output/

# Specify output file
pbRecorder-cli --cli -o recording.mp4
```

### Capture Modes

```bash
# Full screen (specific monitor)
pbRecorder-cli --cli --mode screen --monitor 1 -o out.mp4

# Window (title match)
pbRecorder-cli --cli --mode window --window "Chrome" -o out.mp4

# Region
pbRecorder-cli --cli --mode region --region 0,0,1920,1080 -o out.mp4
```

### Video Settings

```bash
# H.264, 60fps, 12Mbps, High profile
pbRecorder-cli --cli --vcodec h264 --container mp4 --fps 60 --vbitrate 12000 \
  --profile high --level 4.1 --hw-encoder -o out.mp4

# WMV, 30fps
pbRecorder-cli --cli --vcodec wmv --fps 30 --vbitrate 5000 -o out.wmv

# MKV container
pbRecorder-cli --cli --vcodec h264 --container mkv -o out.mkv
```

### Audio Settings

```bash
# System audio
pbRecorder-cli --cli --audio-out 0 --acodec aac --abitrate 192 -o out.mp4

# Microphone
pbRecorder-cli --cli --audio-in 0 --acodec aac --abitrate 192 -o mic.mp4

# No audio
pbRecorder-cli --cli --no-audio -o out.mp4

# MKV + Opus
pbRecorder-cli --cli --container mkv --acodec opus --abitrate 128 -o out.mkv

# MKV + PCM (96kHz/24bit)
pbRecorder-cli --cli --container mkv --acodec pcm --sample-rate 96000 --bit-depth 24 -o out.mkv
```

### All CLI Options

Run `pbRecorder-cli --help` for the complete list of options.

## Building from Source

### Prerequisites
- CMake 3.24+
- Qt 6.9+
- MinGW-w64 or MSVC
- (Optional) ASIO SDK — obtained separately from Steinberg. It is not bundled in this repository.

### Build Steps

```bash
mkdir build && cd build
cmake .. -G "MinGW Makefiles" -DCMAKE_PREFIX_PATH="<path-to-Qt6>"
cmake --build . --config Release -- -j4
```

To enable ASIO support, pass an SDK path outside the repository, or place a local copy under the ignored `third_party/asiosdk/` directory:

```bash
cmake .. -G "MinGW Makefiles" -DCMAKE_PREFIX_PATH="<path-to-Qt6>" -DASIO_SDK_DIR="C:/SDKs/asiosdk"
```

If `ASIO_SDK_DIR` does not contain `common/asio.h`, pbRecorder builds without ASIO support.

This produces two executables:
- `pbRecorder.exe` — GUI application (WIN32 subsystem)
- `pbRecorder-cli.exe` — CLI application (CONSOLE subsystem)

Third-party libraries (libebml, libmatroska, libogg, libvorbis, libopus) are automatically downloaded and built via CMake FetchContent.

### Smoke Tests

The project registers lightweight CTest smoke tests when `BUILD_TESTING=ON`:

```bash
ctest --test-dir build -C Release --output-on-failure
```

The tests verify CLI startup with `pbRecorder-cli --help` and GUI startup with `pbRecorder.exe --ui-screenshot <path>`. The GUI test writes an initial-window screenshot and fails if the app hangs or the image is not created.

GitHub Actions runs the same Windows build and smoke tests on pushes and pull requests.

## Tech Stack

- **UI**: Qt 6 (Widgets)
- **Capture**: DXGI Desktop Duplication API
- **Encoding**: Media Foundation (H.264/AAC/MP3/WMV/WMA)
- **MKV container**: libmatroska + libebml (native implementation)
- **Audio capture**: WASAPI (loopback or mic), optional ASIO
- **Audio codecs**: libopus, libvorbis (for MKV)

## Encoding & Licensing

pbRecorder uses a **patent and license-clean architecture**.

### MP4 (H.264 + AAC/MP3)
- Encoded using Windows built-in **Media Foundation**
- No codec libraries are bundled — uses OS-provided H.264/AAC encoders
- **No FFmpeg, x264, or other GPL/LGPL codec libraries**

### MKV (H.264 + AAC/Opus/Vorbis/PCM)
- Video: Media Foundation generates raw H.264 NALUs, written directly to MKV via **libmatroska/libebml**
- Audio: AAC uses the Windows AAC MFT directly; Opus uses **libopus** (BSD), Vorbis uses **libvorbis** (BSD)
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
| Steinberg ASIO SDK | Optional, external | Steinberg license | ASIO host support; not bundled |

- **No GPL contamination**: No GPL/AGPL libraries are used
- **No FFmpeg**: Codec processing does not use FFmpeg
- Vendor SDK payloads such as the Steinberg ASIO SDK are intentionally not committed to this repository

## License

MIT License
