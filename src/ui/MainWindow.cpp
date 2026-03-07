#include "MainWindow.h"
#include "ui_MainWindow.h"

#include "RegionSelectorWidget.h"
#include "SettingsDialog.h"

#include "core/MonitorEnumerator.h"
#include "core/WindowEnumerator.h"
#include "core/RecordingSession.h"
#include "audio/WasapiCapture.h"
#include "audio/AsioCapture.h"
#include "pipeline/IRecordingPipeline.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QCloseEvent>
#include <QStandardPaths>
#include <QDateTime>
#include <QDebug>
#include <QSettings>
#include <QInputDialog>
#include <QDir>
#include <QFileInfo>

// ============================================================================
// Construction / Destruction
// ============================================================================

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(std::make_unique<Ui::MainWindow>())
    , monitorEnum_(std::make_unique<pb::MonitorEnumerator>())
    , windowEnum_(std::make_unique<pb::WindowEnumerator>())
{
    ui->setupUi(this);

    setupConnections();

    // Initial population
    populateMonitors();
    populateWindows();
    populateAudioDevices();

    // Initialize combo state
    onVideoCodecChanged(0);          // triggers updateContainerCombo
    onCaptureModeChanged(0);         // show/hide correct widgets

    // Default output path: Output folder next to exe
    QString outputDir = QCoreApplication::applicationDirPath() + "/Output";
    QDir().mkpath(outputDir);
    QString defaultFile = outputDir + "/recording_"
                          + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")
                          + ".mp4";
    ui->outputPathEdit->setText(defaultFile);

    // Load presets and restore last session
    loadPresets();
    loadSettings();

    statusBar()->showMessage(tr("準備完了"));

    // Auto-test mode: --auto-test records for 5 seconds then exits
    if (QCoreApplication::arguments().contains("--auto-test")) {
        QTimer::singleShot(1000, this, [this]() {
            onRecord(); // start recording
            QTimer::singleShot(5000, this, [this]() {
                onRecord(); // stop recording
                QTimer::singleShot(2000, this, [this]() {
                    close(); // exit
                });
            });
        });
    }
}

MainWindow::~MainWindow() = default;

// ============================================================================
// Event overrides
// ============================================================================

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (isRecording_) {
        auto ret = QMessageBox::question(
            this, tr("録画中"),
            tr("録画中です。録画を停止して終了しますか？"),
            QMessageBox::Yes | QMessageBox::No);
        if (ret == QMessageBox::No) {
            event->ignore();
            return;
        }
        onRecord(); // stop
    }
    saveSettings();
    event->accept();
}

// ============================================================================
// Signal / Slot connections
// ============================================================================

void MainWindow::setupConnections()
{
    // Capture mode
    connect(ui->captureModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onCaptureModeChanged);

    // Video codec / container
    connect(ui->videoCodecCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onVideoCodecChanged);
    connect(ui->containerCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onContainerChanged);

    // Video bitrate sync
    connect(ui->videoBitrateSlider, &QSlider::valueChanged,
            this, &MainWindow::onVideoBitrateSliderChanged);
    connect(ui->videoBitrateSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &MainWindow::onVideoBitrateSpinBoxChanged);

    // Audio bitrate sync
    connect(ui->audioBitrateSlider, &QSlider::valueChanged,
            this, &MainWindow::onAudioBitrateSliderChanged);
    connect(ui->audioBitrateSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &MainWindow::onAudioBitrateSpinBoxChanged);

    // ASIO channel visibility
    connect(ui->inputAudioCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
        bool isAsio = false;
        if (index > 0 && (index - 1) < static_cast<int>(inputAudioDevices_.size())) {
            isAsio = (inputAudioDevices_[index - 1].type == pb::AudioDeviceType::ASIO);
        }
        ui->asioChannelLabel->setVisible(isAsio);
        ui->asioStartChSpin->setVisible(isAsio);
        ui->asioEndChSpin->setVisible(isAsio);
    });

    // Initially hide ASIO channel controls
    ui->asioChannelLabel->setVisible(false);
    ui->asioStartChSpin->setVisible(false);
    ui->asioEndChSpin->setVisible(false);

    // Audio codec-specific settings visibility
    connect(ui->audioCodecCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onAudioCodecChanged);

    // Default sample rate to 48000 Hz
    ui->audioSampleRateCombo->setCurrentIndex(1);

    // Initially update audio codec-specific widgets
    updateAudioCodecWidgets();

    // Buttons
    connect(ui->refreshWindowsBtn, &QPushButton::clicked,
            this, &MainWindow::onRefreshWindows);
    connect(ui->selectRegionBtn, &QPushButton::clicked,
            this, &MainWindow::onSelectRegion);
    connect(ui->browseBtn, &QPushButton::clicked,
            this, &MainWindow::onBrowse);
    connect(ui->recordBtn, &QPushButton::clicked,
            this, &MainWindow::onRecord);
    ui->recordBtn->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    connect(ui->pauseBtn, &QPushButton::clicked,
            this, &MainWindow::onPause);

    // Menu
    connect(ui->actionSettings, &QAction::triggered,
            this, &MainWindow::onSettingsTriggered);
    connect(ui->actionExit, &QAction::triggered,
            this, &QMainWindow::close);

    // Preset controls
    connect(ui->presetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onPresetChanged);
    connect(ui->savePresetBtn, &QPushButton::clicked,
            this, &MainWindow::onSavePreset);
    connect(ui->deletePresetBtn, &QPushButton::clicked,
            this, &MainWindow::onDeletePreset);

    // Update timer
    connect(&updateTimer_, &QTimer::timeout,
            this, &MainWindow::onUpdateTimer);
}

// ============================================================================
// Capture mode
// ============================================================================

void MainWindow::onCaptureModeChanged(int index)
{
    updateCaptureWidgetVisibility(index);
}

void MainWindow::updateCaptureWidgetVisibility(int mode)
{
    // 0=Screen, 1=Window, 2=Region
    bool showMonitor = (mode == 0);
    bool showWindow  = (mode == 1);
    bool showRegion  = (mode == 2);

    ui->monitorLabel->setVisible(showMonitor);
    ui->monitorCombo->setVisible(showMonitor);

    ui->windowLabel->setVisible(showWindow);
    ui->windowCombo->setVisible(showWindow);
    ui->refreshWindowsBtn->setVisible(showWindow);

    ui->regionLabel->setVisible(showRegion);
    ui->regionInfoLabel->setVisible(showRegion);
    ui->selectRegionBtn->setVisible(showRegion);
    ui->autoAdjustCheck->setVisible(showRegion);
}

// ============================================================================
// Video codec / container / audio codec filtering
// ============================================================================

void MainWindow::onVideoCodecChanged(int index)
{
    Q_UNUSED(index);
    updateContainerCombo();
}

void MainWindow::updateContainerCombo()
{
    int codecIdx = ui->videoCodecCombo->currentIndex();
    ui->containerCombo->blockSignals(true);
    ui->containerCombo->clear();

    if (codecIdx == 0) {
        // H.264 -> MP4, MKV
        ui->containerCombo->addItem("MP4", static_cast<int>(pb::ContainerFormat::MP4));
        ui->containerCombo->addItem("MKV", static_cast<int>(pb::ContainerFormat::MKV));
    } else {
        // WMV -> WMV only
        ui->containerCombo->addItem("WMV", static_cast<int>(pb::ContainerFormat::WMV));
    }

    ui->containerCombo->blockSignals(false);
    onContainerChanged(ui->containerCombo->currentIndex());
}

void MainWindow::onContainerChanged(int index)
{
    Q_UNUSED(index);
    updateAudioCodecCombo();
    updateOutputExtension();
}

void MainWindow::updateAudioCodecCombo()
{
    if (ui->containerCombo->count() == 0) return;

    auto container = static_cast<pb::ContainerFormat>(
        ui->containerCombo->currentData().toInt());

    ui->audioCodecCombo->blockSignals(true);
    ui->audioCodecCombo->clear();

    switch (container) {
    case pb::ContainerFormat::MP4:
        ui->audioCodecCombo->addItem("AAC",  static_cast<int>(pb::AudioCodec::AAC));
        ui->audioCodecCombo->addItem("MP3",  static_cast<int>(pb::AudioCodec::MP3));
        break;
    case pb::ContainerFormat::MKV:
        ui->audioCodecCombo->addItem("AAC",    static_cast<int>(pb::AudioCodec::AAC));
        ui->audioCodecCombo->addItem("Opus",   static_cast<int>(pb::AudioCodec::Opus));
        ui->audioCodecCombo->addItem("Vorbis", static_cast<int>(pb::AudioCodec::Vorbis));
        ui->audioCodecCombo->addItem("PCM",    static_cast<int>(pb::AudioCodec::PCM));
        break;
    case pb::ContainerFormat::WMV:
        ui->audioCodecCombo->addItem("WMA", static_cast<int>(pb::AudioCodec::WMA));
        break;
    }

    ui->audioCodecCombo->blockSignals(false);
    updateAudioCodecWidgets();
}

void MainWindow::updateOutputExtension()
{
    if (ui->containerCombo->count() == 0) return;

    auto container = static_cast<pb::ContainerFormat>(
        ui->containerCombo->currentData().toInt());

    QString path = ui->outputPathEdit->text();
    if (path.isEmpty()) return;

    // Replace extension
    int dotPos = path.lastIndexOf('.');
    if (dotPos > 0) {
        path = path.left(dotPos);
    }

    switch (container) {
    case pb::ContainerFormat::MP4: path += ".mp4"; break;
    case pb::ContainerFormat::MKV: path += ".mkv"; break;
    case pb::ContainerFormat::WMV: path += ".wmv"; break;
    }

    ui->outputPathEdit->setText(path);
}

// ============================================================================
// Bitrate slider <-> spinbox sync
// ============================================================================

void MainWindow::onVideoBitrateSliderChanged(int value)
{
    ui->videoBitrateSpinBox->blockSignals(true);
    ui->videoBitrateSpinBox->setValue(value);
    ui->videoBitrateSpinBox->blockSignals(false);
}

void MainWindow::onVideoBitrateSpinBoxChanged(int value)
{
    ui->videoBitrateSlider->blockSignals(true);
    ui->videoBitrateSlider->setValue(value);
    ui->videoBitrateSlider->blockSignals(false);
}

void MainWindow::onAudioBitrateSliderChanged(int value)
{
    ui->audioBitrateSpinBox->blockSignals(true);
    ui->audioBitrateSpinBox->setValue(value);
    ui->audioBitrateSpinBox->blockSignals(false);
}

void MainWindow::onAudioBitrateSpinBoxChanged(int value)
{
    ui->audioBitrateSlider->blockSignals(true);
    ui->audioBitrateSlider->setValue(value);
    ui->audioBitrateSlider->blockSignals(false);
}

// ============================================================================
// Audio enable/disable
// ============================================================================

void MainWindow::onRecordAudioToggled(bool /*checked*/)
{
    // No longer used - audio recording controlled by device selection
}

void MainWindow::onAudioCodecChanged(int /*index*/)
{
    updateAudioCodecWidgets();
}

void MainWindow::updateAudioCodecWidgets()
{
    int codecData = -1;
    if (ui->audioCodecCombo->count() > 0) {
        codecData = ui->audioCodecCombo->currentData().toInt();
    }
    auto codec = static_cast<pb::AudioCodec>(codecData);

    bool showBitrate = (codec == pb::AudioCodec::AAC ||
                        codec == pb::AudioCodec::MP3 ||
                        codec == pb::AudioCodec::WMA ||
                        codec == pb::AudioCodec::Opus);
    bool showPcm = (codec == pb::AudioCodec::PCM);
    bool showVorbisQuality = (codec == pb::AudioCodec::Vorbis);

    ui->audioBitrateLabel->setVisible(showBitrate);
    ui->audioBitrateSlider->setVisible(showBitrate);
    ui->audioBitrateSpinBox->setVisible(showBitrate);

    ui->audioSampleRateLabel->setVisible(showPcm);
    ui->audioSampleRateCombo->setVisible(showPcm);
    ui->audioBitDepthLabel->setVisible(showPcm);
    ui->audioBitDepthCombo->setVisible(showPcm);

    ui->vorbisQualityLabel->setVisible(showVorbisQuality);
    ui->vorbisQualitySlider->setVisible(showVorbisQuality);
}

// ============================================================================
// Populate monitors / windows / audio devices
// ============================================================================

void MainWindow::populateMonitors()
{
    ui->monitorCombo->clear();
    try {
        monitors_ = monitorEnum_->enumerate();
        for (const auto& m : monitors_) {
            QString label = QString::fromStdWString(m.name)
                            + QString(" (%1x%2)").arg(m.width).arg(m.height);
            ui->monitorCombo->addItem(label);
        }
    } catch (const std::exception& e) {
        qWarning() << "Failed to enumerate monitors:" << e.what();
    }
}

void MainWindow::populateWindows()
{
    ui->windowCombo->clear();
    try {
        windows_ = windowEnum_->enumerate();
        for (const auto& w : windows_) {
            QString title = QString::fromStdWString(w.title);
            QString proc  = QString::fromStdWString(w.processName);
            QString label = title;
            if (!proc.isEmpty()) {
                label = proc + " - " + title;
            }
            // Truncate for display
            if (label.length() > 80) {
                label = label.left(77) + "...";
            }
            ui->windowCombo->addItem(label);
        }
    } catch (const std::exception& e) {
        qWarning() << "Failed to enumerate windows:" << e.what();
    }
}

void MainWindow::populateAudioDevices()
{
    ui->outputAudioCombo->clear();
    ui->inputAudioCombo->clear();
    outputAudioDevices_.clear();
    inputAudioDevices_.clear();

    try {
        auto wasapiDevs = pb::WasapiCapture::enumerateDevices();
        for (auto& d : wasapiDevs) {
            if (d.type == pb::AudioDeviceType::WASAPI_Render) {
                outputAudioDevices_.push_back(std::move(d));
            } else if (d.type == pb::AudioDeviceType::WASAPI_Capture) {
                inputAudioDevices_.push_back(std::move(d));
            }
        }
    } catch (const std::exception& e) {
        qWarning() << "WASAPI enumeration failed:" << e.what();
    }

    try {
        auto asioDevs = pb::AsioCapture::enumerateDevices();
        for (auto& d : asioDevs) {
            inputAudioDevices_.push_back(std::move(d));
        }
    } catch (const std::exception& e) {
        qWarning() << "ASIO enumeration failed:" << e.what();
    }

    // Output devices (system audio / speakers)
    ui->outputAudioCombo->addItem(tr("なし"));
    for (const auto& dev : outputAudioDevices_) {
        ui->outputAudioCombo->addItem(QString::fromStdWString(dev.name));
    }
    // Default: select first device if available
    if (!outputAudioDevices_.empty()) {
        ui->outputAudioCombo->setCurrentIndex(1);
    }

    // Input devices (microphones + ASIO)
    ui->inputAudioCombo->addItem(tr("なし"));
    for (const auto& dev : inputAudioDevices_) {
        QString prefix;
        if (dev.type == pb::AudioDeviceType::ASIO) prefix = "[ASIO] ";
        ui->inputAudioCombo->addItem(prefix + QString::fromStdWString(dev.name));
    }
}

// ============================================================================
// Buttons: refresh, select region, browse
// ============================================================================

void MainWindow::onRefreshWindows()
{
    populateWindows();
    statusBar()->showMessage(tr("ウィンドウ一覧を更新しました"), 3000);
}

void MainWindow::onSelectRegion()
{
    auto *selector = new RegionSelectorWidget();
    selector->setAutoAdjust(ui->autoAdjustCheck->isChecked());
    connect(selector, &RegionSelectorWidget::regionSelected,
            this, [this](int x, int y, int w, int h) {
        selectedRegion_ = {x, y, w, h};
        regionSelected_ = true;
        ui->regionInfoLabel->setText(
            QString("%1,%2  %3x%4").arg(x).arg(y).arg(w).arg(h));
        ui->regionInfoLabel->setStyleSheet("");
        statusBar()->showMessage(tr("範囲を選択しました"), 3000);
    });
    connect(selector, &RegionSelectorWidget::selectionCancelled,
            this, [this]() {
        statusBar()->showMessage(tr("範囲選択がキャンセルされました"), 3000);
    });
    selector->show();
}

void MainWindow::onBrowse()
{
    auto container = pb::ContainerFormat::MP4;
    if (ui->containerCombo->count() > 0) {
        container = static_cast<pb::ContainerFormat>(
            ui->containerCombo->currentData().toInt());
    }

    QString filter = getFilterForContainer(container);

    // ダイアログに渡すパスのディレクトリが存在しなければデフォルトに戻す
    QString initialPath = ui->outputPathEdit->text();
    QFileInfo fi(initialPath);
    if (!fi.dir().exists()) {
        QString defaultDir = QCoreApplication::applicationDirPath() + "/Output";
        QDir().mkpath(defaultDir);
        initialPath = defaultDir + "/recording.mp4";
    }

    QString path = QFileDialog::getSaveFileName(
        this, tr("録画を保存"), initialPath, filter);

    if (!path.isEmpty()) {
        ui->outputPathEdit->setText(path);
    }
}

QString MainWindow::getFilterForContainer(pb::ContainerFormat fmt) const
{
    switch (fmt) {
    case pb::ContainerFormat::MP4: return tr("MP4 動画 (*.mp4)");
    case pb::ContainerFormat::MKV: return tr("MKV 動画 (*.mkv)");
    case pb::ContainerFormat::WMV: return tr("WMV 動画 (*.wmv)");
    }
    return tr("すべてのファイル (*.*)");
}

// ============================================================================
// Record / Pause
// ============================================================================

void MainWindow::onRecord()
{
    if (!isRecording_) {
        // Validate
        if (ui->outputPathEdit->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, tr("エラー"), tr("出力ファイルパスを指定してください。"));
            return;
        }

        if (ui->captureModeCombo->currentIndex() == 2 && !regionSelected_) {
            QMessageBox::warning(this, tr("エラー"), tr("先にキャプチャ範囲を選択してください。"));
            return;
        }

        pb::RecordingConfig config = buildRecordingConfig();

        // Create and initialize recording session
        session_ = std::make_unique<pb::RecordingSession>();
        errorShown_ = false;
        session_->setErrorCallback([this](const std::string& error) {
            QMetaObject::invokeMethod(this, [this, error]() {
                statusBar()->showMessage(QString::fromStdString("Error: " + error));
                if (!errorShown_) {
                    errorShown_ = true;
                    QMessageBox::critical(this, tr("録画エラー"),
                        QString::fromStdString(error));
                    if (isRecording_) {
                        onRecord(); // stop
                    }
                }
            }, Qt::QueuedConnection);
        });

        if (!session_->initialize(config)) {
            QMessageBox::critical(this, tr("エラー"),
                tr("録画セッションの初期化に失敗しました。\nキャプチャソースとオーディオデバイスが利用可能か確認してください。"));
            session_.reset();
            return;
        }

        if (!session_->start()) {
            QMessageBox::critical(this, tr("エラー"), tr("録画の開始に失敗しました。"));
            session_.reset();
            return;
        }

        setRecordingState(true);
        statusBar()->showMessage(tr("録画を開始しました"));
    } else {
        // Stop recording
        if (session_) {
            session_->stop();
            session_.reset();
        }

        setRecordingState(false);
        statusBar()->showMessage(tr("録画を停止しました"), 5000);
    }
}

void MainWindow::onPause()
{
    if (!isRecording_ || !session_) return;

    if (!isPaused_) {
        session_->pause();
        pauseStartMs_ = recordingElapsed_.elapsed();
        setPausedState(true);
        statusBar()->showMessage(tr("録画を一時停止しました"));
    } else {
        session_->resume();
        pausedAccumMs_ += (recordingElapsed_.elapsed() - pauseStartMs_);
        setPausedState(false);
        statusBar()->showMessage(tr("録画を再開しました"));
    }
}

void MainWindow::setRecordingState(bool recording)
{
    isRecording_ = recording;
    isPaused_ = false;
    pausedAccumMs_ = 0;

    if (recording) {
        ui->recordBtn->setText(tr("停止"));
        ui->recordBtn->setStyleSheet(
            "QPushButton { background-color: #336699; color: white; border-radius: 6px; }"
            "QPushButton:hover { background-color: #4477aa; }");
        ui->pauseBtn->setEnabled(true);
        ui->pauseBtn->setText(tr("一時停止"));
        recordingElapsed_.start();
        updateTimer_.start(100);

        // Disable settings during recording
        ui->sourceGroupBox->setEnabled(false);
        ui->videoGroupBox->setEnabled(false);
        ui->audioGroupBox->setEnabled(false);
        ui->outputGroupBox->setEnabled(false);
    } else {
        ui->recordBtn->setText(tr("録画"));
        ui->recordBtn->setStyleSheet(
            "QPushButton { background-color: #cc3333; color: white; border-radius: 6px; }"
            "QPushButton:hover { background-color: #ee4444; }"
            "QPushButton:disabled { background-color: #888888; }");
        ui->pauseBtn->setEnabled(false);
        ui->pauseBtn->setText(tr("一時停止"));
        updateTimer_.stop();

        ui->sourceGroupBox->setEnabled(true);
        ui->videoGroupBox->setEnabled(true);
        ui->audioGroupBox->setEnabled(true);
        ui->outputGroupBox->setEnabled(true);
    }
}

void MainWindow::setPausedState(bool paused)
{
    isPaused_ = paused;
    ui->pauseBtn->setText(paused ? tr("再開") : tr("一時停止"));
}

// ============================================================================
// Timer update (duration & file size)
// ============================================================================

void MainWindow::onUpdateTimer()
{
    if (!isRecording_) return;

    int64_t elapsedMs = recordingElapsed_.elapsed();
    int64_t activeMs = elapsedMs - pausedAccumMs_;
    if (isPaused_) {
        activeMs -= (recordingElapsed_.elapsed() - pauseStartMs_);
    }
    if (activeMs < 0) activeMs = 0;

    ui->durationLabel->setText(formatDuration(activeMs));

    // Update file size: use actual file size if available, otherwise estimate
    if (session_) {
        int64_t fileSize = session_->getFileSize();
        // MP4/WMV containers buffer data until Finalize, so file size stays
        // very small during recording. Use estimate if file < 4KB.
        if (fileSize < 4096 && activeMs > 0) {
            // Estimate: (video bitrate + audio bitrate) * duration
            int64_t totalBitrate = ui->videoBitrateSpinBox->value() * 1000LL;
            if (ui->outputAudioCombo->currentIndex() > 0 || ui->inputAudioCombo->currentIndex() > 0) {
                totalBitrate += ui->audioBitrateSpinBox->value() * 1000LL;
            }
            fileSize = (totalBitrate / 8) * activeMs / 1000;
            ui->fileSizeLabel->setText("~" + formatFileSize(fileSize));
        } else {
            ui->fileSizeLabel->setText(formatFileSize(fileSize));
        }
    }
}

// ============================================================================
// Build config from UI
// ============================================================================

pb::RecordingConfig MainWindow::buildRecordingConfig() const
{
    pb::RecordingConfig config;

    // Capture
    int modeIdx = ui->captureModeCombo->currentIndex();
    config.capture.mode = static_cast<pb::CaptureMode>(modeIdx);

    if (modeIdx == 0 && ui->monitorCombo->currentIndex() >= 0
        && ui->monitorCombo->currentIndex() < static_cast<int>(monitors_.size())) {
        config.capture.monitorIndex = ui->monitorCombo->currentIndex();
    }

    if (modeIdx == 1 && ui->windowCombo->currentIndex() >= 0
        && ui->windowCombo->currentIndex() < static_cast<int>(windows_.size())) {
        config.capture.targetWindow = windows_[ui->windowCombo->currentIndex()].hwnd;
    }

    if (modeIdx == 2 && regionSelected_) {
        config.capture.region = selectedRegion_;
    }

    // Video
    config.video.codec = (ui->videoCodecCombo->currentIndex() == 0)
                         ? pb::VideoCodec::H264
                         : pb::VideoCodec::WMV;
    config.video.fps = ui->fpsSpinBox->value();
    config.video.bitrate = ui->videoBitrateSpinBox->value() * 1000;  // kbps -> bps
    config.video.quality = ui->videoQualitySlider->value();
    config.video.realtimeEncode = ui->realtimeEncodeCheck->isChecked();

    // Container
    if (ui->containerCombo->count() > 0) {
        config.container = static_cast<pb::ContainerFormat>(
            ui->containerCombo->currentData().toInt());
    }

    // Audio
    if (ui->audioCodecCombo->count() > 0) {
        config.audio.codec = static_cast<pb::AudioCodec>(
            ui->audioCodecCombo->currentData().toInt());
    }
    config.audio.bitrate = ui->audioBitrateSpinBox->value() * 1000;  // kbps -> bps

    // PCM-specific settings
    if (config.audio.codec == pb::AudioCodec::PCM) {
        static const int sampleRates[] = {44100, 48000, 96000};
        int srIdx = ui->audioSampleRateCombo->currentIndex();
        if (srIdx >= 0 && srIdx < 3) {
            config.audio.sampleRate = sampleRates[srIdx];
        }
        static const int bitDepths[] = {16, 24, 32};
        int bdIdx = ui->audioBitDepthCombo->currentIndex();
        if (bdIdx >= 0 && bdIdx < 3) {
            config.audio.bitsPerSample = bitDepths[bdIdx];
        }
    }

    // Vorbis quality (0-10 -> 0-100)
    if (config.audio.codec == pb::AudioCodec::Vorbis) {
        config.audio.quality = ui->vorbisQualitySlider->value() * 10;
    }

    // Output device (system audio)
    int outIdx = ui->outputAudioCombo->currentIndex();
    if (outIdx > 0 && (outIdx - 1) < static_cast<int>(outputAudioDevices_.size())) {
        config.useOutputAudio = true;
        config.outputAudioDevice = outputAudioDevices_[outIdx - 1];
    } else {
        config.useOutputAudio = false;
    }

    // Input device (microphone / ASIO)
    int inIdx = ui->inputAudioCombo->currentIndex();
    if (inIdx > 0 && (inIdx - 1) < static_cast<int>(inputAudioDevices_.size())) {
        config.useInputAudio = true;
        config.inputAudioDevice = inputAudioDevices_[inIdx - 1];
        // ASIO channel range (UI is 1-based, internal is 0-based)
        if (config.inputAudioDevice.type == pb::AudioDeviceType::ASIO) {
            config.inputAudioDevice.asioStartChannel = ui->asioStartChSpin->value() - 1;
            config.inputAudioDevice.asioEndChannel = ui->asioEndChSpin->value() - 1;
        }
    } else {
        config.useInputAudio = false;
    }

    config.recordAudio = config.useOutputAudio || config.useInputAudio;

    // Output
    config.outputPath = ui->outputPathEdit->text().toStdWString();

    return config;
}

// ============================================================================
// Settings dialog
// ============================================================================

void MainWindow::onSettingsTriggered()
{
    SettingsDialog dlg(this);
    if (dlg.exec() == QDialog::Accepted) {
        // Apply settings if needed
        statusBar()->showMessage(tr("設定を適用しました"), 3000);
    }
}

// ============================================================================
// Formatting helpers
// ============================================================================

QString MainWindow::formatDuration(int64_t ms) const
{
    int totalSec = static_cast<int>(ms / 1000);
    int hours   = totalSec / 3600;
    int minutes = (totalSec % 3600) / 60;
    int seconds = totalSec % 60;
    return QString("%1:%2:%3")
        .arg(hours, 2, 10, QChar('0'))
        .arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 2, 10, QChar('0'));
}

QString MainWindow::formatFileSize(int64_t bytes) const
{
    if (bytes < 1024)
        return QString("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    if (bytes < 1024LL * 1024 * 1024)
        return QString("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
    return QString("%1 GB").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
}

// ============================================================================
// Preset System
// ============================================================================

void MainWindow::loadPresets()
{
    ui->presetCombo->blockSignals(true);
    ui->presetCombo->clear();
    ui->presetCombo->addItem(tr("(カスタム)"));

    QSettings settings("pbRecorder", "pbRecorder");
    settings.beginGroup("Presets");
    QStringList presets = settings.childGroups();
    for (const QString& name : presets) {
        ui->presetCombo->addItem(name);
    }
    settings.endGroup();

    ui->presetCombo->blockSignals(false);
}

void MainWindow::onSavePreset()
{
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("プリセット保存"),
                                         tr("プリセット名:"), QLineEdit::Normal,
                                         QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    name = name.trimmed();

    saveCurrentAsPreset(name);
    loadPresets();

    // Select the newly saved preset
    int idx = ui->presetCombo->findText(name);
    if (idx >= 0) {
        ui->presetCombo->setCurrentIndex(idx);
    }

    statusBar()->showMessage(tr("プリセット '%1' を保存しました").arg(name), 3000);
}

void MainWindow::saveCurrentAsPreset(const QString& name)
{
    QSettings settings("pbRecorder", "pbRecorder");
    settings.beginGroup("Presets/" + name);

    settings.setValue("captureMode", ui->captureModeCombo->currentIndex());
    settings.setValue("videoCodec", ui->videoCodecCombo->currentIndex());
    settings.setValue("container", ui->containerCombo->currentIndex());
    settings.setValue("fps", ui->fpsSpinBox->value());
    settings.setValue("videoBitrate", ui->videoBitrateSpinBox->value());
    settings.setValue("videoQuality", ui->videoQualitySlider->value());
    settings.setValue("audioCodec", ui->audioCodecCombo->currentIndex());
    settings.setValue("audioBitrate", ui->audioBitrateSpinBox->value());
    settings.setValue("outputAudioIndex", ui->outputAudioCombo->currentIndex());
    settings.setValue("inputAudioIndex", ui->inputAudioCombo->currentIndex());
    settings.setValue("realtimeEncode", ui->realtimeEncodeCheck->isChecked());

    settings.endGroup();
}

void MainWindow::onDeletePreset()
{
    int idx = ui->presetCombo->currentIndex();
    if (idx <= 0) {
        // Cannot delete "(Custom)"
        QMessageBox::information(this, tr("プリセット削除"),
                                 tr("削除するプリセットを選択してください。"));
        return;
    }

    QString name = ui->presetCombo->currentText();
    auto ret = QMessageBox::question(this, tr("プリセット削除"),
                                     tr("プリセット '%1' を削除しますか？").arg(name),
                                     QMessageBox::Yes | QMessageBox::No);
    if (ret != QMessageBox::Yes) return;

    QSettings settings("pbRecorder", "pbRecorder");
    settings.beginGroup("Presets");
    settings.remove(name);
    settings.endGroup();

    loadPresets();
    statusBar()->showMessage(tr("プリセット '%1' を削除しました").arg(name), 3000);
}

void MainWindow::onPresetChanged(int index)
{
    if (index <= 0) return; // "(Custom)" or invalid
    QString name = ui->presetCombo->itemText(index);
    applyPreset(name);
}

void MainWindow::applyPreset(const QString& name)
{
    QSettings settings("pbRecorder", "pbRecorder");
    settings.beginGroup("Presets/" + name);

    if (settings.contains("captureMode"))
        ui->captureModeCombo->setCurrentIndex(settings.value("captureMode").toInt());

    if (settings.contains("videoCodec"))
        ui->videoCodecCombo->setCurrentIndex(settings.value("videoCodec").toInt());

    if (settings.contains("container"))
        ui->containerCombo->setCurrentIndex(settings.value("container").toInt());

    if (settings.contains("fps"))
        ui->fpsSpinBox->setValue(settings.value("fps").toInt());

    if (settings.contains("videoBitrate")) {
        int vb = settings.value("videoBitrate").toInt();
        ui->videoBitrateSpinBox->setValue(vb);
        ui->videoBitrateSlider->setValue(vb);
    }

    if (settings.contains("videoQuality"))
        ui->videoQualitySlider->setValue(settings.value("videoQuality").toInt());

    if (settings.contains("audioCodec"))
        ui->audioCodecCombo->setCurrentIndex(settings.value("audioCodec").toInt());

    if (settings.contains("audioBitrate")) {
        int ab = settings.value("audioBitrate").toInt();
        ui->audioBitrateSpinBox->setValue(ab);
        ui->audioBitrateSlider->setValue(ab);
    }

    if (settings.contains("outputAudioIndex")) {
        int idx = settings.value("outputAudioIndex").toInt();
        if (idx >= 0 && idx < ui->outputAudioCombo->count())
            ui->outputAudioCombo->setCurrentIndex(idx);
    }
    if (settings.contains("inputAudioIndex")) {
        int idx = settings.value("inputAudioIndex").toInt();
        if (idx >= 0 && idx < ui->inputAudioCombo->count())
            ui->inputAudioCombo->setCurrentIndex(idx);
    }

    if (settings.contains("realtimeEncode"))
        ui->realtimeEncodeCheck->setChecked(settings.value("realtimeEncode").toBool());

    settings.endGroup();
}

// ============================================================================
// Window State Persistence
// ============================================================================

void MainWindow::saveSettings()
{
    QSettings settings("pbRecorder", "pbRecorder");

    // Window geometry
    settings.setValue("window/geometry", saveGeometry());

    // Last session parameters
    settings.beginGroup("lastSession");
    settings.setValue("captureMode", ui->captureModeCombo->currentIndex());
    settings.setValue("monitorIndex", ui->monitorCombo->currentIndex());
    settings.setValue("windowIndex", ui->windowCombo->currentIndex());
    settings.setValue("regionSelected", regionSelected_);
    if (regionSelected_) {
        settings.setValue("regionX", selectedRegion_.x);
        settings.setValue("regionY", selectedRegion_.y);
        settings.setValue("regionW", selectedRegion_.width);
        settings.setValue("regionH", selectedRegion_.height);
    }
    settings.setValue("videoCodec", ui->videoCodecCombo->currentIndex());
    settings.setValue("container", ui->containerCombo->currentIndex());
    settings.setValue("fps", ui->fpsSpinBox->value());
    settings.setValue("videoBitrate", ui->videoBitrateSpinBox->value());
    settings.setValue("videoQuality", ui->videoQualitySlider->value());
    settings.setValue("audioCodec", ui->audioCodecCombo->currentIndex());
    settings.setValue("audioBitrate", ui->audioBitrateSpinBox->value());
    settings.setValue("outputAudioIndex", ui->outputAudioCombo->currentIndex());
    settings.setValue("inputAudioIndex", ui->inputAudioCombo->currentIndex());
    settings.setValue("realtimeEncode", ui->realtimeEncodeCheck->isChecked());
    settings.setValue("asioStartCh", ui->asioStartChSpin->value());
    settings.setValue("asioEndCh", ui->asioEndChSpin->value());

    // Output directory (just the directory part)
    QString outputPath = ui->outputPathEdit->text();
    int lastSlash = outputPath.lastIndexOf('/');
    if (lastSlash < 0) lastSlash = outputPath.lastIndexOf('\\');
    if (lastSlash > 0) {
        settings.setValue("outputDir", outputPath.left(lastSlash));
    }
    settings.endGroup();
}

void MainWindow::loadSettings()
{
    QSettings settings("pbRecorder", "pbRecorder");

    // Restore window geometry
    if (settings.contains("window/geometry")) {
        restoreGeometry(settings.value("window/geometry").toByteArray());
    }

    // Restore last session parameters
    settings.beginGroup("lastSession");

    if (settings.contains("captureMode"))
        ui->captureModeCombo->setCurrentIndex(settings.value("captureMode").toInt());

    if (settings.contains("monitorIndex")) {
        int idx = settings.value("monitorIndex").toInt();
        if (idx >= 0 && idx < ui->monitorCombo->count())
            ui->monitorCombo->setCurrentIndex(idx);
    }
    if (settings.contains("windowIndex")) {
        int idx = settings.value("windowIndex").toInt();
        if (idx >= 0 && idx < ui->windowCombo->count())
            ui->windowCombo->setCurrentIndex(idx);
    }
    if (settings.value("regionSelected", false).toBool()) {
        regionSelected_ = true;
        selectedRegion_.x = settings.value("regionX", 0).toInt();
        selectedRegion_.y = settings.value("regionY", 0).toInt();
        selectedRegion_.width = settings.value("regionW", 800).toInt();
        selectedRegion_.height = settings.value("regionH", 600).toInt();
        ui->regionInfoLabel->setText(
            QString("%1x%2 @ (%3,%4)")
                .arg(selectedRegion_.width).arg(selectedRegion_.height)
                .arg(selectedRegion_.x).arg(selectedRegion_.y));
        ui->regionInfoLabel->setStyleSheet("");
    }

    if (settings.contains("videoCodec"))
        ui->videoCodecCombo->setCurrentIndex(settings.value("videoCodec").toInt());

    if (settings.contains("container"))
        ui->containerCombo->setCurrentIndex(settings.value("container").toInt());

    if (settings.contains("fps"))
        ui->fpsSpinBox->setValue(settings.value("fps").toInt());

    if (settings.contains("videoBitrate")) {
        int vb = settings.value("videoBitrate").toInt();
        ui->videoBitrateSpinBox->setValue(vb);
        ui->videoBitrateSlider->setValue(vb);
    }

    if (settings.contains("videoQuality"))
        ui->videoQualitySlider->setValue(settings.value("videoQuality").toInt());

    if (settings.contains("audioCodec"))
        ui->audioCodecCombo->setCurrentIndex(settings.value("audioCodec").toInt());

    if (settings.contains("audioBitrate")) {
        int ab = settings.value("audioBitrate").toInt();
        ui->audioBitrateSpinBox->setValue(ab);
        ui->audioBitrateSlider->setValue(ab);
    }

    if (settings.contains("outputAudioIndex")) {
        int idx = settings.value("outputAudioIndex").toInt();
        if (idx >= 0 && idx < ui->outputAudioCombo->count())
            ui->outputAudioCombo->setCurrentIndex(idx);
    }
    if (settings.contains("inputAudioIndex")) {
        int idx = settings.value("inputAudioIndex").toInt();
        if (idx >= 0 && idx < ui->inputAudioCombo->count())
            ui->inputAudioCombo->setCurrentIndex(idx);
    }

    if (settings.contains("asioStartCh"))
        ui->asioStartChSpin->setValue(settings.value("asioStartCh").toInt());
    if (settings.contains("asioEndCh"))
        ui->asioEndChSpin->setValue(settings.value("asioEndCh").toInt());

    if (settings.contains("realtimeEncode"))
        ui->realtimeEncodeCheck->setChecked(settings.value("realtimeEncode").toBool());

    if (settings.contains("outputDir")) {
        QString dir = settings.value("outputDir").toString();
        // 保存されたディレクトリが存在しない場合はデフォルト（exe横のOutput）にフォールバック
        if (!QDir(dir).exists()) {
            dir = QCoreApplication::applicationDirPath() + "/Output";
            QDir().mkpath(dir);
        }
        QString defaultFile = dir + "/recording_"
                              + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")
                              + ".mp4";
        ui->outputPathEdit->setText(defaultFile);
    }

    // Update extension to match the selected container
    updateOutputExtension();

    settings.endGroup();
}
