#include "cli/CliRunner.h"

#include "core/RecordingSession.h"
#include "core/Types.h"
#include "core/MonitorEnumerator.h"
#include "core/WindowEnumerator.h"
#include "audio/WasapiCapture.h"
#include "audio/AsioCapture.h"

#include <QCoreApplication>
#include <QTextStream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>

#include <windows.h>
#include <atomic>

// ---------------------------------------------------------------------------
// Ctrl+C handler
// ---------------------------------------------------------------------------
static std::atomic<bool> g_stopRequested{false};

static BOOL WINAPI consoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT) {
        g_stopRequested.store(true);
        return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Helpers (file-local)
// ---------------------------------------------------------------------------

static QTextStream& out() {
    static QTextStream s(stdout);
    return s;
}

static QTextStream& err() {
    static QTextStream s(stderr);
    return s;
}

static QString formatDuration(int64_t ms) {
    int sec = static_cast<int>(ms / 1000);
    int h = sec / 3600;
    int m = (sec % 3600) / 60;
    int s = sec % 60;
    return QString("%1:%2:%3")
        .arg(h, 2, 10, QChar('0'))
        .arg(m, 2, 10, QChar('0'))
        .arg(s, 2, 10, QChar('0'));
}

static QString formatSize(int64_t bytes) {
    if (bytes < 1024)
        return QString("%1 B").arg(bytes);
    double kb = bytes / 1024.0;
    if (kb < 1024.0)
        return QString("%1 KB").arg(kb, 0, 'f', 1);
    double mb = kb / 1024.0;
    if (mb < 1024.0)
        return QString("%1 MB").arg(mb, 0, 'f', 1);
    double gb = mb / 1024.0;
    return QString("%1 GB").arg(gb, 0, 'f', 2);
}

static QString containerExtension(pb::ContainerFormat fmt) {
    switch (fmt) {
    case pb::ContainerFormat::MP4: return ".mp4";
    case pb::ContainerFormat::MKV: return ".mkv";
    case pb::ContainerFormat::WMV: return ".wmv";
    }
    return ".mp4";
}

static QString videoCodecName(pb::VideoCodec c) {
    switch (c) {
    case pb::VideoCodec::H264: return "H264";
    case pb::VideoCodec::WMV:  return "WMV";
    }
    return "H264";
}

static QString audioCodecName(pb::AudioCodec c) {
    switch (c) {
    case pb::AudioCodec::AAC:    return "AAC";
    case pb::AudioCodec::MP3:    return "MP3";
    case pb::AudioCodec::Opus:   return "OPUS";
    case pb::AudioCodec::Vorbis: return "VORBIS";
    case pb::AudioCodec::PCM:    return "PCM";
    case pb::AudioCodec::WMA:    return "WMA";
    }
    return "AAC";
}

static QString generateAutoFileName(const pb::RecordingConfig& cfg) {
    QString dateTime = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss");
    QString vc = videoCodecName(cfg.video.codec);
    int vbr = cfg.video.bitrate / 1000; // bps -> kbps
    int fps = cfg.video.fps;
    QString ac = audioCodecName(cfg.audio.codec);
    int abr = cfg.audio.bitrate / 1000;
    QString ext = containerExtension(cfg.container);

    return QString("%1_%2-%3K-%4FPS_%5-%6K%7")
        .arg(dateTime).arg(vc).arg(vbr).arg(fps).arg(ac).arg(abr).arg(ext);
}

// Load pbRecorder.json next to the executable
static QJsonObject loadSettingsJson() {
    QString path = QCoreApplication::applicationDirPath() + "/pbRecorder.json";
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    return doc.isObject() ? doc.object() : QJsonObject();
}

// Find audio device by index or partial name match
static bool findAudioDevice(const std::vector<pb::AudioDeviceInfo>& devices,
                            const QString& spec, pb::AudioDeviceInfo& result) {
    bool isIndex = false;
    int idx = spec.toInt(&isIndex);
    if (isIndex && idx >= 0 && idx < static_cast<int>(devices.size())) {
        result = devices[idx];
        return true;
    }
    // Partial name match
    for (const auto& d : devices) {
        if (QString::fromStdWString(d.name).contains(spec, Qt::CaseInsensitive)) {
            result = d;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// isCliMode
// ---------------------------------------------------------------------------
bool CliRunner::isCliMode(const QStringList& args) {
    static const QStringList cliFlags = {
        "--cli", "--list-monitors", "--list-windows",
        "--list-audio-devices", "--help"
    };
    for (const auto& a : args) {
        if (cliFlags.contains(a))
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// printUsage
// ---------------------------------------------------------------------------
void CliRunner::printUsage() {
    out() << "pbRecorder CLI\n"
          << "\n"
          << "Usage: pbRecorder.exe --cli [options]\n"
          << "\n"
          << "Required:\n"
          << "  --cli                  CLIモードで起動\n"
          << "\n"
          << "General:\n"
          << "  --help                 このヘルプを表示\n"
          << "  --list-monitors        モニター一覧を表示\n"
          << "  --list-windows         ウィンドウ一覧を表示\n"
          << "  --list-audio-devices   オーディオデバイス一覧を表示\n"
          << "\n"
          << "Output:\n"
          << "  -o, --output PATH      出力パス (ファイルまたはディレクトリ)\n"
          << "  --auto-name            自動ファイル名生成\n"
          << "\n"
          << "Capture:\n"
          << "  --mode MODE            screen|window|region (default: screen)\n"
          << "  --monitor N            モニターインデックス (default: 0)\n"
          << "  --window TITLE         ウィンドウタイトル (部分一致)\n"
          << "  --hwnd HEX             ウィンドウハンドル (16進数)\n"
          << "  --region X,Y,W,H       範囲指定\n"
          << "  --cursor               カーソルキャプチャ有効 (default)\n"
          << "  --no-cursor            カーソルキャプチャ無効\n"
          << "\n"
          << "Video:\n"
          << "  --vcodec CODEC         h264|wmv (default: h264)\n"
          << "  --container FMT        mp4|mkv|wmv (default: mp4)\n"
          << "  --fps N                フレームレート (default: 60)\n"
          << "  --vbitrate N           映像ビットレートkbps (default: 8000)\n"
          << "  --vquality N           映像品質 0-100 (default: 70)\n"
          << "  --hw-encoder           HWエンコーダー使用 (default)\n"
          << "  --no-hw-encoder        SWエンコーダー使用\n"
          << "  --realtime             リアルタイムエンコード (default)\n"
          << "  --no-realtime          非リアルタイムエンコード\n"
          << "\n"
          << "Audio:\n"
          << "  --audio-out DEV        出力オーディオデバイス (インデックスまたは名前部分一致)\n"
          << "  --audio-in DEV         入力オーディオデバイス (インデックスまたは名前部分一致)\n"
          << "  --no-audio             音声なし\n"
          << "  --acodec CODEC         aac|mp3|opus|vorbis|pcm|wma (default: aac)\n"
          << "  --abitrate N           音声ビットレートkbps (default: 192)\n"
          << "  --sample-rate N        サンプルレートHz (default: 48000)\n"
          << "  --bit-depth N          ビット深度 (default: 16)\n"
          << "  --vorbis-quality N     Vorbis品質 0-10 (default: 5)\n"
          << "  --asio-channels S-E    ASIOチャンネル範囲 (例: 1-2)\n"
          << "\n"
          << "Recording:\n"
          << "  --duration N           録画秒数 (省略時はCtrl+Cで停止)\n"
          << "  --preset NAME          プリセット名で設定読み込み\n"
          << Qt::endl;
}

// ---------------------------------------------------------------------------
// listMonitors
// ---------------------------------------------------------------------------
int CliRunner::listMonitors() {
    pb::MonitorEnumerator enumerator;
    auto monitors = enumerator.enumerate();

    out() << "Monitors:" << Qt::endl;
    for (const auto& m : monitors) {
        out() << QString("  %1: %2 (%3x%4) at (%5,%6)")
                     .arg(m.index)
                     .arg(QString::fromStdWString(m.name))
                     .arg(m.width).arg(m.height)
                     .arg(m.x).arg(m.y)
              << Qt::endl;
    }
    out().flush();
    return 0;
}

// ---------------------------------------------------------------------------
// listWindows
// ---------------------------------------------------------------------------
int CliRunner::listWindows() {
    pb::WindowEnumerator enumerator;
    auto windows = enumerator.enumerate();

    out() << "Windows:" << Qt::endl;
    int idx = 0;
    for (const auto& w : windows) {
        out() << QString("  %1: [%2] %3 (HWND: 0x%4)")
                     .arg(idx++)
                     .arg(QString::fromStdWString(w.processName))
                     .arg(QString::fromStdWString(w.title))
                     .arg(reinterpret_cast<quintptr>(w.hwnd), 8, 16, QChar('0'))
              << Qt::endl;
    }
    out().flush();
    return 0;
}

// ---------------------------------------------------------------------------
// listAudioDevices
// ---------------------------------------------------------------------------
int CliRunner::listAudioDevices() {
    auto wasapiDevices = pb::WasapiCapture::enumerateDevices();
    auto asioDevices = pb::AsioCapture::enumerateDevices();

    // Split into output and input
    out() << "Output devices (speakers/loopback):" << Qt::endl;
    int idx = 0;
    for (const auto& d : wasapiDevices) {
        if (d.type == pb::AudioDeviceType::WASAPI_Render) {
            out() << QString("  %1: %2")
                         .arg(idx).arg(QString::fromStdWString(d.name))
                  << Qt::endl;
            ++idx;
        }
    }
    for (const auto& d : asioDevices) {
        if (d.type == pb::AudioDeviceType::ASIO_Output) {
            out() << QString("  %1: [ASIO] %2")
                         .arg(idx).arg(QString::fromStdWString(d.name))
                  << Qt::endl;
            ++idx;
        }
    }

    out() << "\nInput devices (microphones):" << Qt::endl;
    idx = 0;
    for (const auto& d : wasapiDevices) {
        if (d.type == pb::AudioDeviceType::WASAPI_Capture) {
            out() << QString("  %1: %2")
                         .arg(idx).arg(QString::fromStdWString(d.name))
                  << Qt::endl;
            ++idx;
        }
    }
    for (const auto& d : asioDevices) {
        if (d.type == pb::AudioDeviceType::ASIO) {
            out() << QString("  %1: [ASIO] %2")
                         .arg(idx).arg(QString::fromStdWString(d.name))
                  << Qt::endl;
            ++idx;
        }
    }

    out().flush();
    return 0;
}

// ---------------------------------------------------------------------------
// run
// ---------------------------------------------------------------------------
int CliRunner::run(const QStringList& args) {
    SetConsoleCtrlHandler(consoleHandler, TRUE);

    // Handle list/help commands (no --cli required)
    if (args.contains("--list-monitors"))      return listMonitors();
    if (args.contains("--list-windows"))        return listWindows();
    if (args.contains("--list-audio-devices"))  return listAudioDevices();
    if (args.contains("--help"))               { printUsage(); return 0; }

    // -----------------------------------------------------------------------
    // Parse arguments
    // -----------------------------------------------------------------------
    pb::RecordingConfig config;
    config.capture.captureCursor = true;
    config.video.useHardwareEncoder = true;
    config.video.realtimeEncode = true;

    QString outputPath;
    bool autoName = false;
    int duration = -1; // seconds, -1 = unlimited
    QString windowTitle;
    QString hwndStr;
    QString presetName;
    bool noAudio = false;
    QString audioOutSpec, audioInSpec;
    QString asioChannels;
    int vorbisQuality = 5;

    auto getArg = [&](int i) -> QString {
        return (i + 1 < args.size()) ? args[i + 1] : QString();
    };

    for (int i = 1; i < args.size(); ++i) {
        const QString& a = args[i];

        if (a == "--cli") continue;

        // Output
        if (a == "-o" || a == "--output")   { outputPath = getArg(i); ++i; continue; }
        if (a == "--auto-name")             { autoName = true; continue; }

        // Capture mode
        if (a == "--mode") {
            QString m = getArg(i).toLower(); ++i;
            if (m == "screen")      config.capture.mode = pb::CaptureMode::Screen;
            else if (m == "window") config.capture.mode = pb::CaptureMode::Window;
            else if (m == "region") config.capture.mode = pb::CaptureMode::Region;
            else { err() << "Error: unknown mode '" << m << "'" << Qt::endl; return 1; }
            continue;
        }
        if (a == "--monitor")  { config.capture.monitorIndex = getArg(i).toInt(); ++i; continue; }
        if (a == "--window")   { windowTitle = getArg(i); ++i; config.capture.mode = pb::CaptureMode::Window; continue; }
        if (a == "--hwnd")     { hwndStr = getArg(i); ++i; config.capture.mode = pb::CaptureMode::Window; continue; }
        if (a == "--region") {
            QStringList parts = getArg(i).split(','); ++i;
            if (parts.size() != 4) { err() << "Error: --region requires X,Y,W,H" << Qt::endl; return 1; }
            config.capture.region = { parts[0].toInt(), parts[1].toInt(), parts[2].toInt(), parts[3].toInt() };
            config.capture.mode = pb::CaptureMode::Region;
            continue;
        }
        if (a == "--cursor")    { config.capture.captureCursor = true; continue; }
        if (a == "--no-cursor") { config.capture.captureCursor = false; continue; }

        // Video
        if (a == "--vcodec") {
            QString c = getArg(i).toLower(); ++i;
            if (c == "h264")     config.video.codec = pb::VideoCodec::H264;
            else if (c == "wmv") config.video.codec = pb::VideoCodec::WMV;
            else { err() << "Error: unknown video codec '" << c << "'" << Qt::endl; return 1; }
            continue;
        }
        if (a == "--container") {
            QString c = getArg(i).toLower(); ++i;
            if (c == "mp4")      config.container = pb::ContainerFormat::MP4;
            else if (c == "mkv") config.container = pb::ContainerFormat::MKV;
            else if (c == "wmv") config.container = pb::ContainerFormat::WMV;
            else { err() << "Error: unknown container '" << c << "'" << Qt::endl; return 1; }
            continue;
        }
        if (a == "--fps")           { config.video.fps = getArg(i).toInt(); ++i; continue; }
        if (a == "--vbitrate")      { config.video.bitrate = getArg(i).toInt() * 1000; ++i; continue; }
        if (a == "--vquality")      { config.video.quality = getArg(i).toInt(); ++i; continue; }
        if (a == "--hw-encoder")    { config.video.useHardwareEncoder = true; continue; }
        if (a == "--no-hw-encoder") { config.video.useHardwareEncoder = false; continue; }
        if (a == "--realtime")      { config.video.realtimeEncode = true; continue; }
        if (a == "--no-realtime")   { config.video.realtimeEncode = false; continue; }

        // Audio
        if (a == "--audio-out")  { audioOutSpec = getArg(i); ++i; continue; }
        if (a == "--audio-in")   { audioInSpec = getArg(i); ++i; continue; }
        if (a == "--no-audio")   { noAudio = true; continue; }
        if (a == "--acodec") {
            QString c = getArg(i).toLower(); ++i;
            if (c == "aac")        config.audio.codec = pb::AudioCodec::AAC;
            else if (c == "mp3")   config.audio.codec = pb::AudioCodec::MP3;
            else if (c == "opus")  config.audio.codec = pb::AudioCodec::Opus;
            else if (c == "vorbis") config.audio.codec = pb::AudioCodec::Vorbis;
            else if (c == "pcm")   config.audio.codec = pb::AudioCodec::PCM;
            else if (c == "wma")   config.audio.codec = pb::AudioCodec::WMA;
            else { err() << "Error: unknown audio codec '" << c << "'" << Qt::endl; return 1; }
            continue;
        }
        if (a == "--abitrate")       { config.audio.bitrate = getArg(i).toInt() * 1000; ++i; continue; }
        if (a == "--sample-rate")    { config.audio.sampleRate = getArg(i).toInt(); ++i; continue; }
        if (a == "--bit-depth")      { config.audio.bitsPerSample = getArg(i).toInt(); ++i; continue; }
        if (a == "--vorbis-quality") { vorbisQuality = getArg(i).toInt(); ++i; continue; }
        if (a == "--asio-channels")  { asioChannels = getArg(i); ++i; continue; }

        // Recording
        if (a == "--duration") { duration = getArg(i).toInt(); ++i; continue; }
        if (a == "--preset")  { presetName = getArg(i); ++i; continue; }

        // Ignored (profile/level are not in Types.h but accept them silently)
        if (a == "--profile" || a == "--level") { ++i; continue; }

        err() << "Error: unknown option '" << a << "'" << Qt::endl;
        return 1;
    }

    // -----------------------------------------------------------------------
    // Apply preset if specified
    // -----------------------------------------------------------------------
    if (!presetName.isEmpty()) {
        QJsonObject root = loadSettingsJson();
        QJsonObject presets = root["presets"].toObject();
        QJsonObject p = presets[presetName].toObject();
        if (p.isEmpty()) {
            err() << "Error: preset '" << presetName << "' not found" << Qt::endl;
            return 1;
        }
        // Map preset indices to enum values
        if (p.contains("videoCodec"))
            config.video.codec = static_cast<pb::VideoCodec>(p["videoCodec"].toInt());
        if (p.contains("container"))
            config.container = static_cast<pb::ContainerFormat>(p["container"].toInt());
        if (p.contains("fps"))
            config.video.fps = p["fps"].toInt();
        if (p.contains("videoBitrate"))
            config.video.bitrate = p["videoBitrate"].toInt() * 1000;
        if (p.contains("videoQuality"))
            config.video.quality = p["videoQuality"].toInt();
        if (p.contains("captureMode"))
            config.capture.mode = static_cast<pb::CaptureMode>(p["captureMode"].toInt());
        if (p.contains("audioCodec"))
            config.audio.codec = static_cast<pb::AudioCodec>(p["audioCodec"].toInt());
        if (p.contains("audioBitrate"))
            config.audio.bitrate = p["audioBitrate"].toInt() * 1000;
        if (p.contains("realtimeEncode"))
            config.video.realtimeEncode = p["realtimeEncode"].toBool();
        if (p.contains("hwEncoder"))
            config.video.useHardwareEncoder = p["hwEncoder"].toBool();
        if (p.contains("captureCursor"))
            config.capture.captureCursor = p["captureCursor"].toBool();
    }

    // -----------------------------------------------------------------------
    // Resolve window target
    // -----------------------------------------------------------------------
    if (config.capture.mode == pb::CaptureMode::Window) {
        if (!hwndStr.isEmpty()) {
            bool ok = false;
            quintptr h = hwndStr.toULongLong(&ok, 16);
            if (!ok) {
                err() << "Error: invalid HWND '" << hwndStr << "'" << Qt::endl;
                return 1;
            }
            config.capture.targetWindow = reinterpret_cast<HWND>(h);
        } else if (!windowTitle.isEmpty()) {
            pb::WindowEnumerator enumerator;
            auto windows = enumerator.findByTitle(windowTitle.toStdWString());
            if (windows.empty()) {
                err() << "Error: no window matching '" << windowTitle << "'" << Qt::endl;
                return 1;
            }
            config.capture.targetWindow = windows[0].hwnd;
            out() << "Matched window: " << QString::fromStdWString(windows[0].title) << Qt::endl;
        } else {
            err() << "Error: --window or --hwnd is required for window mode" << Qt::endl;
            return 1;
        }
    }

    // -----------------------------------------------------------------------
    // Resolve audio devices
    // -----------------------------------------------------------------------
    if (noAudio) {
        config.recordAudio = false;
        config.useOutputAudio = false;
        config.useInputAudio = false;
    } else {
        auto wasapiDevices = pb::WasapiCapture::enumerateDevices();
        auto asioDevices = pb::AsioCapture::enumerateDevices();

        // Split by type
        std::vector<pb::AudioDeviceInfo> renderDevices, captureDevices;
        for (const auto& d : wasapiDevices) {
            if (d.type == pb::AudioDeviceType::WASAPI_Render)
                renderDevices.push_back(d);
            else if (d.type == pb::AudioDeviceType::WASAPI_Capture)
                captureDevices.push_back(d);
        }
        for (const auto& d : asioDevices) {
            if (d.type == pb::AudioDeviceType::ASIO_Output)
                renderDevices.push_back(d);
            else if (d.type == pb::AudioDeviceType::ASIO)
                captureDevices.push_back(d);
        }

        if (!audioOutSpec.isEmpty()) {
            if (!findAudioDevice(renderDevices, audioOutSpec, config.outputAudioDevice)) {
                err() << "Error: output audio device '" << audioOutSpec << "' not found" << Qt::endl;
                return 1;
            }
            config.useOutputAudio = true;
            config.recordAudio = true;
        }
        if (!audioInSpec.isEmpty()) {
            if (!findAudioDevice(captureDevices, audioInSpec, config.inputAudioDevice)) {
                err() << "Error: input audio device '" << audioInSpec << "' not found" << Qt::endl;
                return 1;
            }
            config.useInputAudio = true;
            config.recordAudio = true;
        }

        if (config.useOutputAudio && config.useInputAudio) {
            err() << "Error: recording system audio and microphone together is not supported yet. "
                  << "Specify only one of --audio-out or --audio-in." << Qt::endl;
            return 1;
        }

        // ASIO channel range
        if (!asioChannels.isEmpty()) {
            QStringList parts = asioChannels.split('-');
            if (parts.size() == 2) {
                int startChannel = std::max(0, parts[0].toInt() - 1);
                int endChannel = std::max(startChannel, parts[1].toInt() - 1);
                if (config.useOutputAudio) {
                    config.outputAudioDevice.asioStartChannel = startChannel;
                    config.outputAudioDevice.asioEndChannel = endChannel;
                }
                if (config.useInputAudio) {
                    config.inputAudioDevice.asioStartChannel = startChannel;
                    config.inputAudioDevice.asioEndChannel = endChannel;
                }
            }
        }

        // Default: if no audio device specified but audio not disabled, use first render device
        if (!noAudio && !config.useOutputAudio && !config.useInputAudio) {
            if (!renderDevices.empty()) {
                config.outputAudioDevice = renderDevices[0];
                config.useOutputAudio = true;
                config.recordAudio = true;
            }
        }
    }

    // Vorbis quality
    if (config.audio.codec == pb::AudioCodec::Vorbis) {
        config.audio.quality = vorbisQuality * 10; // map 0-10 to 0-100
    }

    // -----------------------------------------------------------------------
    // Output path
    // -----------------------------------------------------------------------
    if (outputPath.isEmpty()) {
        // Default to Output directory next to exe
        outputPath = QCoreApplication::applicationDirPath() + "/Output";
        autoName = true;
    }

    QFileInfo fi(outputPath);
    if (fi.isDir() || autoName) {
        QString dir = fi.isDir() ? outputPath : fi.absolutePath();
        QDir().mkpath(dir);
        QString fileName = generateAutoFileName(config);
        config.outputPath = QDir(dir).filePath(fileName).toStdWString();
    } else {
        QDir().mkpath(fi.absolutePath());
        config.outputPath = QDir::toNativeSeparators(fi.absoluteFilePath()).toStdWString();
    }

    // -----------------------------------------------------------------------
    // Initialize and start recording
    // -----------------------------------------------------------------------
    pb::RecordingSession session;
    std::atomic<bool> errorOccurred{false};
    QString errorMsg;

    session.setErrorCallback([&](const std::string& error) {
        if (!errorMsg.isEmpty()) {
            errorMsg += "\n";
        }
        errorMsg += QString::fromStdString(error);
        errorOccurred.store(true);
        g_stopRequested.store(true);
    });

    if (!session.initialize(config)) {
        err() << "Error: failed to initialize recording session" << Qt::endl;
        if (!errorMsg.isEmpty()) {
            err() << "Detail: " << errorMsg << Qt::endl;
        }
        return 2;
    }

    if (!session.start()) {
        err() << "Error: failed to start recording" << Qt::endl;
        if (!errorMsg.isEmpty()) {
            err() << "Detail: " << errorMsg << Qt::endl;
        }
        return 2;
    }

    out() << "Recording... (Ctrl+C to stop)" << Qt::endl;
    out().flush();

    // -----------------------------------------------------------------------
    // Wait loop
    // -----------------------------------------------------------------------
    auto startTime = std::chrono::steady_clock::now();

    while (!g_stopRequested.load()) {
        QThread::msleep(500);

        int64_t ms = session.getDurationMs();
        int64_t size = session.getFileSize();
        out() << "  " << formatDuration(ms) << "  " << formatSize(size) << Qt::endl;
        out().flush();

        // Check duration limit
        if (duration > 0) {
            auto elapsed = std::chrono::steady_clock::now() - startTime;
            auto elapsedSec = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
            if (elapsedSec >= duration) {
                break;
            }
        }
    }

    // -----------------------------------------------------------------------
    // Stop recording
    // -----------------------------------------------------------------------
    int64_t finalMs = session.getDurationMs();
    session.stop();

    // Get actual file size after stop (flushed to disk)
    int64_t finalSize = 0;
    QFileInfo finalFi(QString::fromStdWString(config.outputPath));
    if (finalFi.exists()) finalSize = finalFi.size();
    QString outFile = QString::fromStdWString(config.outputPath);

    out() << "\n"
          << "Recording stopped. Duration: " << formatDuration(finalMs)
          << ", Size: " << formatSize(finalSize) << Qt::endl;
    out() << "Output: " << outFile << Qt::endl;
    out().flush();

    SetConsoleCtrlHandler(consoleHandler, FALSE);

    if (errorOccurred.load()) {
        err() << "Error during recording: " << errorMsg << Qt::endl;
        return 3;
    }

    return 0;
}
