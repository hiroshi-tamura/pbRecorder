#include "MainWindow.h"
#include "ui_MainWindow.h"

#include "RegionSelectorWidget.h"

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
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QInputDialog>
#include <QDir>
#include <QFile>
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

    // Default H.264 profile to High
    ui->h264ProfileCombo->setCurrentIndex(2);

    // Default output folder: Output folder next to exe
    QString outputDir = QCoreApplication::applicationDirPath() + "/Output";
    QDir().mkpath(outputDir);
    ui->outputDirEdit->setText(QDir::toNativeSeparators(outputDir));
    updateAutoFileName();

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

    // Auto filename checkbox
    connect(ui->autoFileNameCheck, &QCheckBox::toggled,
            this, [this](bool checked) {
        ui->outputFileEdit->setEnabled(!checked);
        if (checked) updateAutoFileName();
    });

    // Update auto filename when settings change
    connect(ui->videoCodecCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::updateAutoFileName);
    connect(ui->containerCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::updateAutoFileName);
    connect(ui->audioCodecCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::updateAutoFileName);
    connect(ui->videoBitrateSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &MainWindow::updateAutoFileName);
    connect(ui->audioBitrateSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &MainWindow::updateAutoFileName);
    connect(ui->fpsSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &MainWindow::updateAutoFileName);

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
    bool isH264 = (index == 0);
    ui->h264ProfileCombo->setVisible(isH264);
    ui->h264LevelCombo->setVisible(isH264);
    ui->h264ProfileLabel->setVisible(isH264);
    ui->h264LevelLabel->setVisible(isH264);
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
    if (ui->autoFileNameCheck->isChecked()) {
        updateAutoFileName();
        return;
    }

    if (ui->containerCombo->count() == 0) return;

    auto container = static_cast<pb::ContainerFormat>(
        ui->containerCombo->currentData().toInt());

    QString fileName = ui->outputFileEdit->text();
    if (fileName.isEmpty()) return;

    // Replace extension
    int dotPos = fileName.lastIndexOf('.');
    if (dotPos > 0) {
        fileName = fileName.left(dotPos);
    }

    switch (container) {
    case pb::ContainerFormat::MP4: fileName += ".mp4"; break;
    case pb::ContainerFormat::MKV: fileName += ".mkv"; break;
    case pb::ContainerFormat::WMV: fileName += ".wmv"; break;
    }

    ui->outputFileEdit->setText(fileName);
}

void MainWindow::updateAutoFileName()
{
    if (!ui->autoFileNameCheck->isChecked()) return;
    ui->outputFileEdit->setText(generateAutoFileName());
}

QString MainWindow::generateAutoFileName() const
{
    QString dateTime = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss");

    // Video codec name
    QString videoCodec;
    if (ui->videoCodecCombo->currentIndex() == 0)
        videoCodec = "H264";
    else
        videoCodec = "WMV";

    int videoBitrate = ui->videoBitrateSpinBox->value();
    int fps = ui->fpsSpinBox->value();

    // Audio codec name
    QString audioCodec;
    if (ui->audioCodecCombo->count() > 0) {
        audioCodec = ui->audioCodecCombo->currentText().toUpper();
    }
    int audioBitrate = ui->audioBitrateSpinBox->value();

    // Extension
    QString ext = ".mp4";
    if (ui->containerCombo->count() > 0) {
        auto container = static_cast<pb::ContainerFormat>(
            ui->containerCombo->currentData().toInt());
        switch (container) {
        case pb::ContainerFormat::MP4: ext = ".mp4"; break;
        case pb::ContainerFormat::MKV: ext = ".mkv"; break;
        case pb::ContainerFormat::WMV: ext = ".wmv"; break;
        }
    }

    return QString("%1_%2-%3K-%4FPS_%5-%6K%7")
        .arg(dateTime)
        .arg(videoCodec)
        .arg(videoBitrate)
        .arg(fps)
        .arg(audioCodec)
        .arg(audioBitrate)
        .arg(ext);
}

QString MainWindow::getOutputFilePath() const
{
    QString dir = ui->outputDirEdit->text();
    QString fileName = ui->outputFileEdit->text();
    return QDir(dir).filePath(fileName);
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
    QString currentDir = ui->outputDirEdit->text();
    if (!QDir(currentDir).exists()) {
        currentDir = QCoreApplication::applicationDirPath() + "/Output";
    }

    QString dir = QFileDialog::getExistingDirectory(
        this, tr("出力フォルダを選択"), currentDir,
        QFileDialog::ShowDirsOnly | QFileDialog::DontUseNativeDialog);

    if (!dir.isEmpty()) {
        ui->outputDirEdit->setText(QDir::toNativeSeparators(dir));
    }
}

// ============================================================================
// Record / Pause
// ============================================================================

void MainWindow::onRecord()
{
    if (!isRecording_) {
        // 自動ファイル名の場合、録画開始時に日時を更新
        if (ui->autoFileNameCheck->isChecked()) {
            updateAutoFileName();
        }

        // Validate
        if (ui->outputDirEdit->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, tr("エラー"), tr("出力フォルダを指定してください。"));
            return;
        }
        if (ui->outputFileEdit->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, tr("エラー"), tr("ファイル名を指定してください。"));
            return;
        }

        // 出力フォルダが存在しなければ作成
        QDir().mkpath(ui->outputDirEdit->text());

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
    config.capture.captureCursor = ui->captureCursorCheck->isChecked();

    // Video
    config.video.codec = (ui->videoCodecCombo->currentIndex() == 0)
                         ? pb::VideoCodec::H264
                         : pb::VideoCodec::WMV;
    config.video.fps = ui->fpsSpinBox->value();
    config.video.bitrate = ui->videoBitrateSpinBox->value() * 1000;  // kbps -> bps
    config.video.quality = ui->videoQualitySlider->value();
    config.video.realtimeEncode = ui->realtimeEncodeCheck->isChecked();
    config.video.useHardwareEncoder = ui->hwEncoderCheck->isChecked();

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
    config.outputPath = getOutputFilePath().toStdWString();

    return config;
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
// JSON Settings I/O
// ============================================================================

QString MainWindow::settingsFilePath() const
{
    return QCoreApplication::applicationDirPath() + "/pbRecorder.json";
}

QJsonObject MainWindow::loadJson() const
{
    QFile file(settingsFilePath());
    if (!file.open(QIODevice::ReadOnly)) return {};
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    return doc.isObject() ? doc.object() : QJsonObject();
}

void MainWindow::saveJson(const QJsonObject& root) const
{
    QFile file(settingsFilePath());
    if (!file.open(QIODevice::WriteOnly)) return;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

// ============================================================================
// Preset System
// ============================================================================

void MainWindow::loadPresets()
{
    ui->presetCombo->blockSignals(true);
    ui->presetCombo->clear();
    ui->presetCombo->addItem(tr("(カスタム)"));

    QJsonObject root = loadJson();
    QJsonObject presets = root["presets"].toObject();
    for (auto it = presets.begin(); it != presets.end(); ++it) {
        ui->presetCombo->addItem(it.key());
    }

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

    int idx = ui->presetCombo->findText(name);
    if (idx >= 0) {
        ui->presetCombo->setCurrentIndex(idx);
    }

    statusBar()->showMessage(tr("プリセット '%1' を保存しました").arg(name), 3000);
}

void MainWindow::saveCurrentAsPreset(const QString& name)
{
    QJsonObject root = loadJson();
    QJsonObject presets = root["presets"].toObject();

    QJsonObject p;
    p["captureMode"] = ui->captureModeCombo->currentIndex();
    p["videoCodec"] = ui->videoCodecCombo->currentIndex();
    p["container"] = ui->containerCombo->currentIndex();
    p["fps"] = ui->fpsSpinBox->value();
    p["videoBitrate"] = ui->videoBitrateSpinBox->value();
    p["videoQuality"] = ui->videoQualitySlider->value();
    p["audioCodec"] = ui->audioCodecCombo->currentIndex();
    p["audioBitrate"] = ui->audioBitrateSpinBox->value();
    p["outputAudioIndex"] = ui->outputAudioCombo->currentIndex();
    p["inputAudioIndex"] = ui->inputAudioCombo->currentIndex();
    p["realtimeEncode"] = ui->realtimeEncodeCheck->isChecked();
    p["hwEncoder"] = ui->hwEncoderCheck->isChecked();
    p["h264Profile"] = ui->h264ProfileCombo->currentIndex();
    p["h264Level"] = ui->h264LevelCombo->currentIndex();
    p["captureCursor"] = ui->captureCursorCheck->isChecked();

    presets[name] = p;
    root["presets"] = presets;
    saveJson(root);
}

void MainWindow::onDeletePreset()
{
    int idx = ui->presetCombo->currentIndex();
    if (idx <= 0) {
        QMessageBox::information(this, tr("プリセット削除"),
                                 tr("削除するプリセットを選択してください。"));
        return;
    }

    QString name = ui->presetCombo->currentText();
    auto ret = QMessageBox::question(this, tr("プリセット削除"),
                                     tr("プリセット '%1' を削除しますか？").arg(name),
                                     QMessageBox::Yes | QMessageBox::No);
    if (ret != QMessageBox::Yes) return;

    QJsonObject root = loadJson();
    QJsonObject presets = root["presets"].toObject();
    presets.remove(name);
    root["presets"] = presets;
    saveJson(root);

    loadPresets();
    statusBar()->showMessage(tr("プリセット '%1' を削除しました").arg(name), 3000);
}

void MainWindow::onPresetChanged(int index)
{
    if (index <= 0) return;
    QString name = ui->presetCombo->itemText(index);
    applyPreset(name);
}

void MainWindow::applyPreset(const QString& name)
{
    QJsonObject root = loadJson();
    QJsonObject p = root["presets"].toObject()[name].toObject();
    if (p.isEmpty()) return;

    if (p.contains("captureMode"))
        ui->captureModeCombo->setCurrentIndex(p["captureMode"].toInt());
    if (p.contains("videoCodec"))
        ui->videoCodecCombo->setCurrentIndex(p["videoCodec"].toInt());
    if (p.contains("container"))
        ui->containerCombo->setCurrentIndex(p["container"].toInt());
    if (p.contains("fps"))
        ui->fpsSpinBox->setValue(p["fps"].toInt());
    if (p.contains("videoBitrate")) {
        int vb = p["videoBitrate"].toInt();
        ui->videoBitrateSpinBox->setValue(vb);
        ui->videoBitrateSlider->setValue(vb);
    }
    if (p.contains("videoQuality"))
        ui->videoQualitySlider->setValue(p["videoQuality"].toInt());
    if (p.contains("audioCodec"))
        ui->audioCodecCombo->setCurrentIndex(p["audioCodec"].toInt());
    if (p.contains("audioBitrate")) {
        int ab = p["audioBitrate"].toInt();
        ui->audioBitrateSpinBox->setValue(ab);
        ui->audioBitrateSlider->setValue(ab);
    }
    if (p.contains("outputAudioIndex")) {
        int i = p["outputAudioIndex"].toInt();
        if (i >= 0 && i < ui->outputAudioCombo->count())
            ui->outputAudioCombo->setCurrentIndex(i);
    }
    if (p.contains("inputAudioIndex")) {
        int i = p["inputAudioIndex"].toInt();
        if (i >= 0 && i < ui->inputAudioCombo->count())
            ui->inputAudioCombo->setCurrentIndex(i);
    }
    if (p.contains("realtimeEncode"))
        ui->realtimeEncodeCheck->setChecked(p["realtimeEncode"].toBool());
    if (p.contains("hwEncoder"))
        ui->hwEncoderCheck->setChecked(p["hwEncoder"].toBool());
    if (p.contains("h264Profile"))
        ui->h264ProfileCombo->setCurrentIndex(p["h264Profile"].toInt());
    if (p.contains("h264Level"))
        ui->h264LevelCombo->setCurrentIndex(p["h264Level"].toInt());
    if (p.contains("captureCursor"))
        ui->captureCursorCheck->setChecked(p["captureCursor"].toBool());
}

// ============================================================================
// Window State Persistence
// ============================================================================

void MainWindow::saveSettings()
{
    QJsonObject root = loadJson();

    // Window geometry (base64 encoded)
    root["windowGeometry"] = QString::fromLatin1(saveGeometry().toBase64());

    QJsonObject s;
    s["captureMode"] = ui->captureModeCombo->currentIndex();
    s["monitorIndex"] = ui->monitorCombo->currentIndex();
    s["windowIndex"] = ui->windowCombo->currentIndex();
    s["regionSelected"] = regionSelected_;
    if (regionSelected_) {
        s["regionX"] = selectedRegion_.x;
        s["regionY"] = selectedRegion_.y;
        s["regionW"] = selectedRegion_.width;
        s["regionH"] = selectedRegion_.height;
    }
    s["videoCodec"] = ui->videoCodecCombo->currentIndex();
    s["container"] = ui->containerCombo->currentIndex();
    s["fps"] = ui->fpsSpinBox->value();
    s["videoBitrate"] = ui->videoBitrateSpinBox->value();
    s["videoQuality"] = ui->videoQualitySlider->value();
    s["audioCodec"] = ui->audioCodecCombo->currentIndex();
    s["audioBitrate"] = ui->audioBitrateSpinBox->value();
    s["outputAudioIndex"] = ui->outputAudioCombo->currentIndex();
    s["inputAudioIndex"] = ui->inputAudioCombo->currentIndex();
    s["realtimeEncode"] = ui->realtimeEncodeCheck->isChecked();
    s["hwEncoder"] = ui->hwEncoderCheck->isChecked();
    s["h264Profile"] = ui->h264ProfileCombo->currentIndex();
    s["h264Level"] = ui->h264LevelCombo->currentIndex();
    s["captureCursor"] = ui->captureCursorCheck->isChecked();
    s["asioStartCh"] = ui->asioStartChSpin->value();
    s["asioEndCh"] = ui->asioEndChSpin->value();
    s["outputDir"] = ui->outputDirEdit->text();
    s["autoFileName"] = ui->autoFileNameCheck->isChecked();
    if (!ui->autoFileNameCheck->isChecked()) {
        s["outputFileName"] = ui->outputFileEdit->text();
    }

    root["lastSession"] = s;
    saveJson(root);
}

void MainWindow::loadSettings()
{
    QJsonObject root = loadJson();

    // Restore window geometry
    if (root.contains("windowGeometry")) {
        QByteArray geo = QByteArray::fromBase64(root["windowGeometry"].toString().toLatin1());
        restoreGeometry(geo);
    }

    QJsonObject s = root["lastSession"].toObject();
    if (s.isEmpty()) return;

    if (s.contains("captureMode"))
        ui->captureModeCombo->setCurrentIndex(s["captureMode"].toInt());

    if (s.contains("monitorIndex")) {
        int idx = s["monitorIndex"].toInt();
        if (idx >= 0 && idx < ui->monitorCombo->count())
            ui->monitorCombo->setCurrentIndex(idx);
    }
    if (s.contains("windowIndex")) {
        int idx = s["windowIndex"].toInt();
        if (idx >= 0 && idx < ui->windowCombo->count())
            ui->windowCombo->setCurrentIndex(idx);
    }
    if (s["regionSelected"].toBool()) {
        regionSelected_ = true;
        selectedRegion_.x = s["regionX"].toInt();
        selectedRegion_.y = s["regionY"].toInt();
        selectedRegion_.width = s["regionW"].toInt(800);
        selectedRegion_.height = s["regionH"].toInt(600);
        ui->regionInfoLabel->setText(
            QString("%1x%2 @ (%3,%4)")
                .arg(selectedRegion_.width).arg(selectedRegion_.height)
                .arg(selectedRegion_.x).arg(selectedRegion_.y));
        ui->regionInfoLabel->setStyleSheet("");
    }

    if (s.contains("videoCodec"))
        ui->videoCodecCombo->setCurrentIndex(s["videoCodec"].toInt());
    if (s.contains("container"))
        ui->containerCombo->setCurrentIndex(s["container"].toInt());
    if (s.contains("fps"))
        ui->fpsSpinBox->setValue(s["fps"].toInt());
    if (s.contains("videoBitrate")) {
        int vb = s["videoBitrate"].toInt();
        ui->videoBitrateSpinBox->setValue(vb);
        ui->videoBitrateSlider->setValue(vb);
    }
    if (s.contains("videoQuality"))
        ui->videoQualitySlider->setValue(s["videoQuality"].toInt());
    if (s.contains("audioCodec"))
        ui->audioCodecCombo->setCurrentIndex(s["audioCodec"].toInt());
    if (s.contains("audioBitrate")) {
        int ab = s["audioBitrate"].toInt();
        ui->audioBitrateSpinBox->setValue(ab);
        ui->audioBitrateSlider->setValue(ab);
    }
    if (s.contains("outputAudioIndex")) {
        int idx = s["outputAudioIndex"].toInt();
        if (idx >= 0 && idx < ui->outputAudioCombo->count())
            ui->outputAudioCombo->setCurrentIndex(idx);
    }
    if (s.contains("inputAudioIndex")) {
        int idx = s["inputAudioIndex"].toInt();
        if (idx >= 0 && idx < ui->inputAudioCombo->count())
            ui->inputAudioCombo->setCurrentIndex(idx);
    }
    if (s.contains("asioStartCh"))
        ui->asioStartChSpin->setValue(s["asioStartCh"].toInt());
    if (s.contains("asioEndCh"))
        ui->asioEndChSpin->setValue(s["asioEndCh"].toInt());
    if (s.contains("realtimeEncode"))
        ui->realtimeEncodeCheck->setChecked(s["realtimeEncode"].toBool());
    if (s.contains("hwEncoder"))
        ui->hwEncoderCheck->setChecked(s["hwEncoder"].toBool());
    if (s.contains("h264Profile"))
        ui->h264ProfileCombo->setCurrentIndex(s["h264Profile"].toInt());
    if (s.contains("h264Level"))
        ui->h264LevelCombo->setCurrentIndex(s["h264Level"].toInt());
    if (s.contains("captureCursor"))
        ui->captureCursorCheck->setChecked(s["captureCursor"].toBool());

    if (s.contains("outputDir")) {
        QString dir = s["outputDir"].toString();
        if (!QDir(dir).exists()) {
            dir = QCoreApplication::applicationDirPath() + "/Output";
            QDir().mkpath(dir);
        }
        ui->outputDirEdit->setText(QDir::toNativeSeparators(dir));
    }

    if (s.contains("autoFileName"))
        ui->autoFileNameCheck->setChecked(s["autoFileName"].toBool());

    if (!ui->autoFileNameCheck->isChecked() && s.contains("outputFileName")) {
        ui->outputFileEdit->setText(s["outputFileName"].toString());
    } else {
        updateAutoFileName();
    }

    ui->outputFileEdit->setEnabled(!ui->autoFileNameCheck->isChecked());
}
