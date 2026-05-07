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
#include <QShowEvent>
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
#include <QStyle>
#include <QPushButton>
#include <QDesktopServices>
#include <QUrl>
#include <QStatusBar>
#include <QRegularExpression>
#include <windows.h>

#include "SettingsDialog.h"
#include "PeakMeterWidget.h"

#include <mmdeviceapi.h>
#include <audioclient.h>

// MinGW's endpointvolume.h only forward-declares IAudioMeterInformation.
// Define the COM interface manually.
#include <endpointvolume.h>

namespace {

UINT nativeHotkeyModifiers(const QKeyCombination& combo)
{
    UINT modifiers = MOD_NOREPEAT;
    const Qt::KeyboardModifiers qtMods = combo.keyboardModifiers();
    if (qtMods.testFlag(Qt::ControlModifier)) modifiers |= MOD_CONTROL;
    if (qtMods.testFlag(Qt::ShiftModifier)) modifiers |= MOD_SHIFT;
    if (qtMods.testFlag(Qt::AltModifier)) modifiers |= MOD_ALT;
    if (qtMods.testFlag(Qt::MetaModifier)) modifiers |= MOD_WIN;
    return modifiers;
}

UINT nativeHotkeyKey(Qt::Key key)
{
    const int value = static_cast<int>(key);
    if ((value >= Qt::Key_A && value <= Qt::Key_Z) ||
        (value >= Qt::Key_0 && value <= Qt::Key_9)) {
        return static_cast<UINT>(value);
    }

    if (value >= Qt::Key_F1 && value <= Qt::Key_F24) {
        return VK_F1 + static_cast<UINT>(value - Qt::Key_F1);
    }

    switch (key) {
    case Qt::Key_Backspace: return VK_BACK;
    case Qt::Key_Tab: return VK_TAB;
    case Qt::Key_Return:
    case Qt::Key_Enter: return VK_RETURN;
    case Qt::Key_Escape: return VK_ESCAPE;
    case Qt::Key_Space: return VK_SPACE;
    case Qt::Key_PageUp: return VK_PRIOR;
    case Qt::Key_PageDown: return VK_NEXT;
    case Qt::Key_End: return VK_END;
    case Qt::Key_Home: return VK_HOME;
    case Qt::Key_Left: return VK_LEFT;
    case Qt::Key_Up: return VK_UP;
    case Qt::Key_Right: return VK_RIGHT;
    case Qt::Key_Down: return VK_DOWN;
    case Qt::Key_Insert: return VK_INSERT;
    case Qt::Key_Delete: return VK_DELETE;
    default: return 0;
    }
}

QString safeFileNameComponent(QString text)
{
    text.replace(QRegularExpression(QStringLiteral(R"([<>:"/\\|?*\x00-\x1F])")), "_");
    text = text.trimmed();
    while (text.endsWith('.') || text.endsWith(' ')) {
        text.chop(1);
    }
    return text.isEmpty() ? QStringLiteral("recording") : text;
}

} // namespace

#ifndef __IAudioMeterInformation_INTERFACE_DEFINED__
#define __IAudioMeterInformation_INTERFACE_DEFINED__

MIDL_INTERFACE("C02216F6-8C67-4B5B-9D00-D008E73E0064")
IAudioMeterInformation : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetPeakValue(float *pfPeak) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMeteringChannelCount(UINT *pnChannelCount) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChannelsPeakValues(UINT32 u32ChannelCount, float *afPeakValues) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryHardwareSupport(DWORD *pdwHardwareSupportMask) = 0;
};

static const GUID IID_IAudioMeterInformation = {
    0xC02216F6, 0x8C67, 0x4B5B,
    {0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64}
};
#endif

// ============================================================================
// Metering Session — keeps WASAPI device active for IAudioMeterInformation
// ============================================================================

struct MainWindow::MeteringSession {
    IAudioMeterInformation* meter = nullptr;
    IAudioClient* client = nullptr;  // keeps capture devices active
    std::wstring deviceId;

    ~MeteringSession() { release(); }

    void release() {
        if (meter) { meter->Release(); meter = nullptr; }
        if (client) { client->Stop(); client->Release(); client = nullptr; }
    }

    float getPeak() {
        if (!meter) return 0.0f;
        float peak = 0.0f;
        meter->GetPeakValue(&peak);
        return peak;
    }

    static std::unique_ptr<MeteringSession> create(const std::wstring& id, bool isCapture) {
        if (id.empty()) return nullptr;

        IMMDeviceEnumerator* enumerator = nullptr;
        HRESULT hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
        if (FAILED(hr) || !enumerator) return nullptr;

        IMMDevice* device = nullptr;
        hr = enumerator->GetDevice(id.c_str(), &device);
        enumerator->Release();
        if (FAILED(hr) || !device) return nullptr;

        auto session = std::make_unique<MeteringSession>();
        session->deviceId = id;

        // For capture devices, create a shared-mode IAudioClient session
        // to keep hardware active so IAudioMeterInformation reports levels
        if (isCapture) {
            IAudioClient* ac = nullptr;
            hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                  nullptr, reinterpret_cast<void**>(&ac));
            if (SUCCEEDED(hr) && ac) {
                WAVEFORMATEX* fmt = nullptr;
                hr = ac->GetMixFormat(&fmt);
                if (SUCCEEDED(hr) && fmt) {
                    hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                        0, 10000000, 0, fmt, nullptr);
                    CoTaskMemFree(fmt);
                    if (SUCCEEDED(hr)) {
                        ac->Start();
                        session->client = ac;
                    } else {
                        ac->Release();
                    }
                } else {
                    ac->Release();
                }
            }
        }

        // Get IAudioMeterInformation
        hr = device->Activate(IID_IAudioMeterInformation, CLSCTX_ALL,
                              nullptr, reinterpret_cast<void**>(&session->meter));
        device->Release();

        if (FAILED(hr) || !session->meter) {
            session->release();
            return nullptr;
        }

        return session;
    }
};

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

    // Preset/settings button icons using Segoe MDL2 Assets (Windows 10+)
    QFont mdl2("Segoe MDL2 Assets", 12);
    for (auto *btn : {ui->overwritePresetBtn, ui->saveAsPresetBtn, ui->deletePresetBtn, ui->settingsBtn}) {
        btn->setFont(mdl2);
        btn->setFixedSize(28, 28);
    }
    ui->overwritePresetBtn->setText(QChar(0xE74E)); // save icon
    ui->saveAsPresetBtn->setText(QChar(0xE710)); // + (add new) icon
    ui->deletePresetBtn->setText(QChar(0xE74D)); // delete icon
    ui->settingsBtn->setText(QChar(0xE713)); // gear icon

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

    // Setup peak meters under audio device combos
    setupPeakMeters();

    // Load presets and restore last session
    loadPresets();
    loadSettings();

    // "Open Folder" button on the status bar (permanent widget)
    openFolderBtn_ = new QPushButton(this);
    openFolderBtn_->setFlat(true);
    openFolderBtn_->setEnabled(false);
    openFolderBtn_->setCursor(Qt::PointingHandCursor);
    connect(openFolderBtn_, &QPushButton::clicked, this, [this]() {
        if (lastSavedPath_.isEmpty()) return;
        QString dir = QFileInfo(lastSavedPath_).absolutePath();
        if (!dir.isEmpty()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
        }
    });
    statusBar()->addPermanentWidget(openFolderBtn_);

    retranslateUi();

    statusBar()->showMessage(currentLang_ == "ja" ? tr("準備完了") : tr("Ready"));

    // Initialize record button guard for current capture mode
    updateRecordButtonGuard();

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

MainWindow::~MainWindow()
{
    unregisterGlobalHotkey();
}

// ============================================================================
// Event overrides
// ============================================================================

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    // Register the global hotkey on first show (winId() must be valid).
    registerGlobalHotkey();
}

bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    if (eventType == "windows_generic_MSG" || eventType == "windows_dispatcher_MSG") {
        MSG *msg = static_cast<MSG *>(message);
        if (msg && msg->message == WM_HOTKEY && msg->wParam == 1) {
            if (isRecording_ || ui->recordBtn->isEnabled()) {
                onRecord();
            }
            if (result) *result = 0;
            return true;
        }
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (isRecording_) {
        const bool ja = (currentLang_ == "ja");
        auto ret = QMessageBox::question(
            this,
            ja ? "録画中" : "Recording",
            ja ? "録画中です。録画を停止して終了しますか？"
               : "Recording is in progress. Stop recording and exit?",
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

    // ASIO channel visibility — separate for output and input
    auto updateAsioOutVisibility = [this]() {
        bool isAsio = false;
        int outIdx = ui->outputAudioCombo->currentIndex();
        if (outIdx > 0 && (outIdx - 1) < static_cast<int>(outputAudioDevices_.size())) {
            auto t = outputAudioDevices_[outIdx - 1].type;
            if (t == pb::AudioDeviceType::ASIO || t == pb::AudioDeviceType::ASIO_Output)
                isAsio = true;
        }
        ui->asioOutChannelLabel->setVisible(isAsio);
        ui->asioOutStartChSpin->setVisible(isAsio);
        ui->asioOutEndChSpin->setVisible(isAsio);
    };
    auto updateAsioInVisibility = [this]() {
        bool isAsio = false;
        int inIdx = ui->inputAudioCombo->currentIndex();
        if (inIdx > 0 && (inIdx - 1) < static_cast<int>(inputAudioDevices_.size())) {
            auto t = inputAudioDevices_[inIdx - 1].type;
            if (t == pb::AudioDeviceType::ASIO || t == pb::AudioDeviceType::ASIO_Output)
                isAsio = true;
        }
        ui->asioChannelLabel->setVisible(isAsio);
        ui->asioStartChSpin->setVisible(isAsio);
        ui->asioEndChSpin->setVisible(isAsio);
    };
    connect(ui->outputAudioCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, updateAsioOutVisibility);
    connect(ui->outputAudioCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this]() { enforceSingleAudioSource(ui->outputAudioCombo); });
    connect(ui->outputAudioCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::validateAudioCodec);
    connect(ui->asioOutStartChSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &MainWindow::validateAudioCodec);
    connect(ui->asioOutEndChSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &MainWindow::validateAudioCodec);
    connect(ui->inputAudioCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, updateAsioInVisibility);
    connect(ui->inputAudioCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this]() { enforceSingleAudioSource(ui->inputAudioCombo); });
    connect(ui->inputAudioCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::validateAudioCodec);
    connect(ui->asioStartChSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &MainWindow::validateAudioCodec);
    connect(ui->asioEndChSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &MainWindow::validateAudioCodec);

    // Initially hide all ASIO channel controls
    ui->asioOutChannelLabel->setVisible(false);
    ui->asioOutStartChSpin->setVisible(false);
    ui->asioOutEndChSpin->setVisible(false);
    ui->asioChannelLabel->setVisible(false);
    ui->asioStartChSpin->setVisible(false);
    ui->asioEndChSpin->setVisible(false);

    // Audio codec-specific settings visibility
    connect(ui->audioCodecCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onAudioCodecChanged);
    connect(ui->audioCodecCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::validateAudioCodec);

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
    connect(ui->openOutputFolderBtn, &QPushButton::clicked,
            this, &MainWindow::onOpenOutputFolder);
    connect(ui->recordBtn, &QPushButton::clicked,
            this, &MainWindow::onRecord);
    ui->recordBtn->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    connect(ui->pauseBtn, &QPushButton::clicked,
            this, &MainWindow::onPause);

    // Preset controls
    connect(ui->presetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onPresetChanged);
    connect(ui->overwritePresetBtn, &QPushButton::clicked,
            this, &MainWindow::onOverwritePreset);
    connect(ui->saveAsPresetBtn, &QPushButton::clicked,
            this, &MainWindow::onSaveAsPreset);
    connect(ui->deletePresetBtn, &QPushButton::clicked,
            this, &MainWindow::onDeletePreset);

    // Settings
    connect(ui->settingsBtn, &QPushButton::clicked,
            this, &MainWindow::onSettingsTriggered);

    // Update timer
    connect(&updateTimer_, &QTimer::timeout,
            this, &MainWindow::onUpdateTimer);

    // Quality slider tooltips: dynamic value display
    auto setupQualityTooltip = [](QSlider *s) {
        if (!s) return;
        s->setToolTip(QString("%1 %").arg(s->value()));
        connect(s, &QSlider::valueChanged, s, [s](int v) {
            s->setToolTip(QString("%1 %").arg(v));
        });
    };
    setupQualityTooltip(ui->videoQualitySlider);
    setupQualityTooltip(ui->vorbisQualitySlider);
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

    updateRecordButtonGuard();
}

void MainWindow::updateRecordButtonGuard()
{
    // Don't touch the record button while recording is in progress
    if (isRecording_) return;

    bool ja = (currentLang_ == "ja");
    int mode = ui->captureModeCombo->currentIndex();

    if (mode == 2 && !regionSelected_) {
        ui->recordBtn->setEnabled(false);
        statusBar()->showMessage(
            ja ? "範囲を選択してください" : "Please select a region first");
    } else if (!audioCodecCompatible_) {
        ui->recordBtn->setEnabled(false);
    } else {
        ui->recordBtn->setEnabled(true);
    }
}

void MainWindow::registerGlobalHotkey()
{
    if (hotkeyRegistered_ || recordHotkey_.isEmpty()) return;

    const QKeyCombination combo = recordHotkey_[0];
    const UINT modifiers = nativeHotkeyModifiers(combo);
    const UINT key = nativeHotkeyKey(combo.key());
    if (key == 0) return;

    if (RegisterHotKey(reinterpret_cast<HWND>(winId()), 1, modifiers, key)) {
        hotkeyRegistered_ = true;
    }
}

void MainWindow::unregisterGlobalHotkey()
{
    if (!hotkeyRegistered_) return;
    UnregisterHotKey(reinterpret_cast<HWND>(winId()), 1);
    hotkeyRegistered_ = false;
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
    validateAudioCodec();
}

void MainWindow::validateAudioCodec()
{
    if (ui->audioCodecCombo->count() == 0) return;

    auto codec = static_cast<pb::AudioCodec>(ui->audioCodecCombo->currentData().toInt());

    // Get max channel count from both output and input audio devices
    int maxUsedChannels = 0;

    // Output device channels
    int outIdx = ui->outputAudioCombo->currentIndex() - 1;
    if (outIdx >= 0 && outIdx < static_cast<int>(outputAudioDevices_.size())) {
        int outCh = outputAudioDevices_[outIdx].channelCount;
        auto type = outputAudioDevices_[outIdx].type;
        if (type == pb::AudioDeviceType::ASIO || type == pb::AudioDeviceType::ASIO_Output) {
            outCh = ui->asioOutEndChSpin->value() - ui->asioOutStartChSpin->value() + 1;
        }
        if (outCh > maxUsedChannels) maxUsedChannels = outCh;
    }

    // Input device channels
    int inIdx = ui->inputAudioCombo->currentIndex() - 1;
    if (inIdx >= 0 && inIdx < static_cast<int>(inputAudioDevices_.size())) {
        int inCh = inputAudioDevices_[inIdx].channelCount;
        auto type = inputAudioDevices_[inIdx].type;
        if (type == pb::AudioDeviceType::ASIO) {
            inCh = ui->asioEndChSpin->value() - ui->asioStartChSpin->value() + 1;
        }
        if (inCh > maxUsedChannels) maxUsedChannels = inCh;
    }

    if (maxUsedChannels == 0) maxUsedChannels = 2;

    // Encoder library limits (not AAC spec limits — MF encoder limits):
    // AAC (Media Foundation): 6ch max (1/2/6 only, Win10+). Spec allows 48ch.
    // MP3 (Media Foundation): 2ch
    // WMA Standard (Media Foundation): 2ch
    // Opus (libopus opus_encoder_create): 2ch. Multistream API allows 255ch.
    // Vorbis (libvorbis): 255ch
    // PCM: no limit
    int maxCodecChannels = 0;
    switch (codec) {
        case pb::AudioCodec::AAC:    maxCodecChannels = 6; break;
        case pb::AudioCodec::MP3:    maxCodecChannels = 2; break;
        case pb::AudioCodec::WMA:    maxCodecChannels = 2; break;
        case pb::AudioCodec::Opus:   maxCodecChannels = 2; break;
        case pb::AudioCodec::Vorbis: maxCodecChannels = 255; break;
        case pb::AudioCodec::PCM:    maxCodecChannels = 0; break;
    }

    bool incompatible = (maxCodecChannels > 0 && maxUsedChannels > maxCodecChannels);
    audioCodecCompatible_ = !incompatible;

    const bool ja = (currentLang_ == "ja");
    if (incompatible) {
        ui->audioCodecCombo->setStyleSheet(
            "QComboBox { border: 2px solid red; color: red; "
            "background-color: #352020; padding: 2px; }"
            "QComboBox::drop-down { border: none; }"
            "QComboBox QAbstractItemView { color: white; background-color: #232323; }");
        ui->audioCodecCombo->setToolTip(
            (ja ? QStringLiteral("%1 は最大 %2ch までです（現在 %3ch）")
                : QStringLiteral("%1 supports up to %2 ch (current %3 ch)"))
                .arg(ui->audioCodecCombo->currentText())
                .arg(maxCodecChannels)
                .arg(maxUsedChannels));
    } else {
        ui->audioCodecCombo->setStyleSheet("");
        ui->audioCodecCombo->setToolTip(ja ? "音声コーデック" : "Audio codec");
    }

    // Disable Record button when codec is incompatible
    if (!isRecording_) {
        updateRecordButtonGuard();
        if (incompatible) {
            statusBar()->showMessage(
                (ja ? QStringLiteral("録音不可: %1 は %2ch に対応していません")
                    : QStringLiteral("Cannot record: %1 does not support %2 ch"))
                    .arg(ui->audioCodecCombo->currentText())
                    .arg(maxUsedChannels));
        }
    }
}

void MainWindow::enforceSingleAudioSource(QObject *changedCombo)
{
    if (ui->outputAudioCombo->currentIndex() <= 0 ||
        ui->inputAudioCombo->currentIndex() <= 0) {
        return;
    }

    const bool ja = (currentLang_ == "ja");
    if (changedCombo == ui->outputAudioCombo) {
        ui->inputAudioCombo->setCurrentIndex(0);
    } else {
        ui->outputAudioCombo->setCurrentIndex(0);
    }

    rebuildMeteringSessions();
    validateAudioCodec();
    statusBar()->showMessage(
        ja ? "現在は出力音声と入力音声の同時録音には対応していません"
           : "System audio and microphone cannot be recorded together yet",
        5000);
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
    case pb::ContainerFormat::MP4:
        fileName += ".mp4";
        break;
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
    QString dateTime = QDateTime::currentDateTime().toString(autoNameFormat_);
    dateTime = safeFileNameComponent(dateTime);

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
    updateAutoFileName();
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
    updateAutoFileName();
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
    updateOutputExtension();
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
            if (d.type == pb::AudioDeviceType::ASIO_Output) {
                outputAudioDevices_.push_back(std::move(d));
            } else {
                inputAudioDevices_.push_back(std::move(d));
            }
        }
    } catch (const std::exception& e) {
        qWarning() << "ASIO enumeration failed:" << e.what();
    }

    const bool ja = (currentLang_ == "ja");
    const QString noneItem = ja ? QStringLiteral("なし") : QStringLiteral("None");

    // Output devices (system audio / speakers / ASIO output)
    ui->outputAudioCombo->addItem(noneItem);
    for (const auto& dev : outputAudioDevices_) {
        QString prefix;
        if (dev.type == pb::AudioDeviceType::ASIO_Output) prefix = "[ASIO] ";
        ui->outputAudioCombo->addItem(prefix + QString::fromStdWString(dev.name));
    }
    // Default: select first device if available
    if (!outputAudioDevices_.empty()) {
        ui->outputAudioCombo->setCurrentIndex(1);
    }

    // Input devices (microphones + ASIO)
    ui->inputAudioCombo->addItem(noneItem);
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
    const bool ja = (currentLang_ == "ja");
    statusBar()->showMessage(
        ja ? "ウィンドウ一覧を更新しました" : "Window list refreshed", 3000);
}

void MainWindow::onSelectRegion()
{
    auto *selector = new RegionSelectorWidget();
    selector->setLanguage(currentLang_);
    selector->setAutoAdjust(ui->autoAdjustCheck->isChecked());
    if (regionSelected_) {
        selector->setInitialRegion(
            selectedRegion_.x, selectedRegion_.y,
            selectedRegion_.width, selectedRegion_.height);
    }
    connect(selector, &RegionSelectorWidget::regionSelected,
            this, [this](int x, int y, int w, int h) {
        selectedRegion_ = {x, y, w, h};
        regionSelected_ = true;
        ui->regionInfoLabel->setText(formatRegionInfo(x, y, w, h));
        ui->regionInfoLabel->setStyleSheet("");
        const bool ja = (currentLang_ == "ja");
        statusBar()->showMessage(
            ja ? "範囲を選択しました" : "Region selected", 3000);
        updateRecordButtonGuard();
    });
    connect(selector, &RegionSelectorWidget::selectionCancelled,
            this, [this]() {
        const bool ja = (currentLang_ == "ja");
        statusBar()->showMessage(
            ja ? "範囲選択がキャンセルされました" : "Region selection cancelled", 3000);
    });
    selector->show();
}

void MainWindow::onBrowse()
{
    QString currentDir = ui->outputDirEdit->text();
    if (!QDir(currentDir).exists()) {
        currentDir = QCoreApplication::applicationDirPath() + "/Output";
    }

    const bool jaBrowse = (currentLang_ == "ja");
    QString dir = QFileDialog::getExistingDirectory(
        this,
        jaBrowse ? "出力フォルダを選択" : "Select output folder",
        currentDir,
        QFileDialog::ShowDirsOnly | QFileDialog::DontUseNativeDialog);

    if (!dir.isEmpty()) {
        ui->outputDirEdit->setText(QDir::toNativeSeparators(dir));
    }
}

void MainWindow::onOpenOutputFolder()
{
    QString dir = ui->outputDirEdit->text().trimmed();
    if (dir.isEmpty()) {
        dir = QCoreApplication::applicationDirPath() + "/Output";
        ui->outputDirEdit->setText(QDir::toNativeSeparators(dir));
    }

    QDir().mkpath(dir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

// ============================================================================
// Record / Pause
// ============================================================================

void MainWindow::onRecord()
{
    const bool ja = (currentLang_ == "ja");
    const QString errTitle = ja ? "エラー" : "Error";

    if (!isRecording_) {
        // 自動ファイル名の場合、録画開始時に日時を更新
        if (ui->autoFileNameCheck->isChecked()) {
            updateAutoFileName();
        }

        // Validate
        if (ui->outputDirEdit->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, errTitle,
                ja ? "出力フォルダを指定してください。"
                   : "Please specify an output folder.");
            return;
        }
        if (ui->outputFileEdit->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, errTitle,
                ja ? "ファイル名を指定してください。"
                   : "Please specify a filename.");
            return;
        }

        // 出力フォルダが存在しなければ作成
        QDir().mkpath(ui->outputDirEdit->text());

        if (ui->captureModeCombo->currentIndex() == 2 && !regionSelected_) {
            QMessageBox::warning(this, errTitle,
                ja ? "先にキャプチャ範囲を選択してください。"
                   : "Please select a capture region first.");
            return;
        }

        // Release metering sessions to avoid WASAPI device conflicts
        releaseMeteringSessions();

        pb::RecordingConfig config = buildRecordingConfig();

        // Create and initialize recording session
        session_ = std::make_unique<pb::RecordingSession>();
        errorShown_ = false;
        session_->setErrorCallback([this, ja](const std::string& error) {
            QMetaObject::invokeMethod(this, [this, ja, error]() {
                statusBar()->showMessage(QString::fromStdString("Error: " + error));
                if (!errorShown_) {
                    errorShown_ = true;
                    QMessageBox::critical(this,
                        ja ? "録画エラー" : "Recording error",
                        QString::fromStdString(error));
                    if (isRecording_) {
                        onRecord(); // stop
                    }
                }
            }, Qt::QueuedConnection);
        });

        if (!session_->initialize(config)) {
            QMessageBox::critical(this, errTitle,
                ja ? "録画セッションの初期化に失敗しました。\nキャプチャソースとオーディオデバイスが利用可能か確認してください。"
                   : "Failed to initialize recording session.\nVerify the capture source and audio device are available.");
            session_.reset();
            rebuildMeteringSessions();
            return;
        }

        if (!session_->start()) {
            QMessageBox::critical(this, errTitle,
                ja ? "録画の開始に失敗しました。" : "Failed to start recording.");
            session_.reset();
            rebuildMeteringSessions();
            return;
        }

        setRecordingState(true);
        statusBar()->showMessage(ja ? "● 録画中…" : "● Recording…");
    } else {
        // Stop recording
        QString outPath = getOutputFilePath();
        int64_t finalSize = 0;
        if (session_) {
            finalSize = session_->getFileSize();
            session_->stop();
            session_.reset();
        }

        setRecordingState(false);

        // Resolve final file size from disk if available
        QFileInfo fi(outPath);
        if (fi.exists()) {
            int64_t diskSize = fi.size();
            if (diskSize > 0) finalSize = diskSize;
            lastSavedPath_ = fi.absoluteFilePath();
            if (openFolderBtn_) openFolderBtn_->setEnabled(true);
            QString name = fi.fileName();
            QString sizeStr = formatFileSize(finalSize);
            statusBar()->showMessage(
                (ja ? QStringLiteral("保存しました: %1 (%2)")
                    : QStringLiteral("Saved: %1 (%2)"))
                    .arg(name).arg(sizeStr));
        } else {
            statusBar()->showMessage(
                ja ? "録画を停止しました" : "Recording stopped",
                5000);
        }

        // Rebuild metering sessions for live meters
        rebuildMeteringSessions();
    }
}

void MainWindow::onPause()
{
    if (!isRecording_ || !session_) return;

    const bool ja = (currentLang_ == "ja");
    if (!isPaused_) {
        session_->pause();
        pauseStartMs_ = recordingElapsed_.elapsed();
        setPausedState(true);
        statusBar()->showMessage(ja ? "‖ 一時停止中" : "‖ Paused");
    } else {
        session_->resume();
        pausedAccumMs_ += (recordingElapsed_.elapsed() - pauseStartMs_);
        setPausedState(false);
        statusBar()->showMessage(ja ? "● 録画中…" : "● Recording…");
    }
}

void MainWindow::setRecordingState(bool recording)
{
    isRecording_ = recording;
    isPaused_ = false;
    pausedAccumMs_ = 0;

    bool ja = (currentLang_ == "ja");

    if (recording) {
        ui->recordBtn->setText(ja ? "停止" : "Stop");
        ui->recordBtn->setStyleSheet(
            "QPushButton { background-color: #336699; color: white; border-radius: 6px; }"
            "QPushButton:hover { background-color: #4477aa; }");
        ui->pauseBtn->setEnabled(true);
        ui->pauseBtn->setText(ja ? "一時停止" : "Pause");
        recordingElapsed_.start();
        updateTimer_.start(100);

        // Title bar reflects recording state
        setWindowTitle(ja ? QString::fromUtf8("● 録画中 - pbRecorder")
                          : QString::fromUtf8("● Recording - pbRecorder"));

        // Disable settings during recording
        ui->sourceGroupBox->setEnabled(false);
        ui->videoGroupBox->setEnabled(false);
        ui->audioGroupBox->setEnabled(false);
        ui->outputGroupBox->setEnabled(false);
    } else {
        ui->recordBtn->setText(ja ? "録画" : "Record");
        ui->recordBtn->setStyleSheet(
            "QPushButton { background-color: #cc3333; color: white; border-radius: 6px; }"
            "QPushButton:hover { background-color: #ee4444; }"
            "QPushButton:disabled { background-color: #888888; }");
        ui->pauseBtn->setEnabled(false);
        ui->pauseBtn->setText(ja ? "一時停止" : "Pause");
        updateTimer_.stop();

        // Reset duration / file size labels to initial values
        ui->durationLabel->setText("00:00:00");
        ui->fileSizeLabel->setText("0 MB");

        // Restore window title
        setWindowTitle("pbRecorder");

        ui->sourceGroupBox->setEnabled(true);
        ui->videoGroupBox->setEnabled(true);
        ui->audioGroupBox->setEnabled(true);
        ui->outputGroupBox->setEnabled(true);

        // Re-evaluate record button guard (e.g. region not selected)
        updateRecordButtonGuard();
    }
}

void MainWindow::setPausedState(bool paused)
{
    isPaused_ = paused;
    const bool ja = (currentLang_ == "ja");
    if (paused) {
        ui->pauseBtn->setText(ja ? "再開" : "Resume");
    } else {
        ui->pauseBtn->setText(ja ? "一時停止" : "Pause");
    }
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

    QString durStr = formatDuration(activeMs);
    ui->durationLabel->setText(durStr);

    // Update window title with current recording/paused state and duration
    bool ja = (currentLang_ == "ja");
    if (isPaused_) {
        setWindowTitle(ja
            ? QString::fromUtf8("‖ 一時停止中 %1 - pbRecorder").arg(durStr)
            : QString::fromUtf8("‖ Paused %1 - pbRecorder").arg(durStr));
    } else {
        setWindowTitle(ja
            ? QString::fromUtf8("● 録画中 %1 - pbRecorder").arg(durStr)
            : QString::fromUtf8("● Recording %1 - pbRecorder").arg(durStr));
    }

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
        // Use DXGI output index (not display sort order) for DxgiScreenCapture
        config.capture.monitorIndex = monitors_[ui->monitorCombo->currentIndex()].dxgiOutputIndex;
    }

    if (modeIdx == 1 && ui->windowCombo->currentIndex() >= 0
        && ui->windowCombo->currentIndex() < static_cast<int>(windows_.size())) {
        config.capture.targetWindow = windows_[ui->windowCombo->currentIndex()].hwnd;
    }

    if (modeIdx == 2 && regionSelected_) {
        config.capture.region = selectedRegion_;
    }
    config.capture.captureCursor = ui->captureCursorCheck->isChecked();
    config.capture.targetFps = ui->fpsSpinBox->value();

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
        // Output ASIO channel range (UI is 1-based, internal is 0-based)
        if (config.outputAudioDevice.type == pb::AudioDeviceType::ASIO_Output ||
            config.outputAudioDevice.type == pb::AudioDeviceType::ASIO) {
            config.outputAudioDevice.asioStartChannel = ui->asioOutStartChSpin->value() - 1;
            config.outputAudioDevice.asioEndChannel = ui->asioOutEndChSpin->value() - 1;
        }
    } else {
        config.useOutputAudio = false;
    }

    // Input device (microphone / ASIO)
    int inIdx = ui->inputAudioCombo->currentIndex();
    if (inIdx > 0 && (inIdx - 1) < static_cast<int>(inputAudioDevices_.size())) {
        config.useInputAudio = true;
        config.inputAudioDevice = inputAudioDevices_[inIdx - 1];
        // Input ASIO channel range (UI is 1-based, internal is 0-based)
        if (config.inputAudioDevice.type == pb::AudioDeviceType::ASIO ||
            config.inputAudioDevice.type == pb::AudioDeviceType::ASIO_Output) {
            config.inputAudioDevice.asioStartChannel = ui->asioStartChSpin->value() - 1;
            config.inputAudioDevice.asioEndChannel = ui->asioEndChSpin->value() - 1;
        }
    } else {
        config.useInputAudio = false;
    }

    config.recordAudio = config.useOutputAudio || config.useInputAudio;
    if (config.useOutputAudio && config.useInputAudio) {
        config.useInputAudio = false;
    }

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

QString MainWindow::formatRegionInfo(int x, int y, int w, int h) const
{
    return QString("%1x%2 @ (%3,%4)").arg(w).arg(h).arg(x).arg(y);
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
    const bool ja = (currentLang_ == "ja");
    ui->presetCombo->blockSignals(true);
    ui->presetCombo->clear();
    ui->presetCombo->addItem(ja ? "(カスタム)" : "(Custom)");

    QJsonObject root = loadJson();
    QJsonObject presets = root["presets"].toObject();
    for (auto it = presets.begin(); it != presets.end(); ++it) {
        ui->presetCombo->addItem(it.key());
    }

    ui->presetCombo->blockSignals(false);
}

void MainWindow::onOverwritePreset()
{
    int idx = ui->presetCombo->currentIndex();
    if (idx <= 0) {
        // (カスタム)選択中は名前をつけて保存にフォールバック
        onSaveAsPreset();
        return;
    }

    QString name = ui->presetCombo->currentText();
    saveCurrentAsPreset(name);
    const bool ja = (currentLang_ == "ja");
    statusBar()->showMessage(
        (ja ? QStringLiteral("プリセット '%1' を上書き保存しました")
            : QStringLiteral("Preset '%1' overwritten")).arg(name), 3000);
}

void MainWindow::onSaveAsPreset()
{
    const bool ja = (currentLang_ == "ja");
    bool ok = false;
    QString name = QInputDialog::getText(this,
                                         ja ? "名前をつけて保存" : "Save as new preset",
                                         ja ? "プリセット名:" : "Preset name:",
                                         QLineEdit::Normal,
                                         QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    name = name.trimmed();

    saveCurrentAsPreset(name);
    loadPresets();

    int idx = ui->presetCombo->findText(name);
    if (idx >= 0) {
        ui->presetCombo->setCurrentIndex(idx);
    }

    statusBar()->showMessage(
        (ja ? QStringLiteral("プリセット '%1' を保存しました")
            : QStringLiteral("Preset '%1' saved")).arg(name), 3000);
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
    const bool ja = (currentLang_ == "ja");
    const QString delTitle = ja ? "プリセット削除" : "Delete preset";

    int idx = ui->presetCombo->currentIndex();
    if (idx <= 0) {
        QMessageBox::information(this, delTitle,
            ja ? "削除するプリセットを選択してください。"
               : "Select a preset to delete.");
        return;
    }

    QString name = ui->presetCombo->currentText();
    auto ret = QMessageBox::question(this, delTitle,
        (ja ? QStringLiteral("プリセット '%1' を削除しますか？")
            : QStringLiteral("Delete preset '%1'?")).arg(name),
        QMessageBox::Yes | QMessageBox::No);
    if (ret != QMessageBox::Yes) return;

    QJsonObject root = loadJson();
    QJsonObject presets = root["presets"].toObject();
    presets.remove(name);
    root["presets"] = presets;
    saveJson(root);

    loadPresets();
    statusBar()->showMessage(
        (ja ? QStringLiteral("プリセット '%1' を削除しました")
            : QStringLiteral("Preset '%1' deleted")).arg(name), 3000);
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
    s["asioOutStartCh"] = ui->asioOutStartChSpin->value();
    s["asioOutEndCh"] = ui->asioOutEndChSpin->value();
    s["asioStartCh"] = ui->asioStartChSpin->value();
    s["asioEndCh"] = ui->asioEndChSpin->value();
    s["outputDir"] = ui->outputDirEdit->text();
    s["autoFileName"] = ui->autoFileNameCheck->isChecked();
    s["autoNameFormat"] = autoNameFormat_;
    s["recordHotkey"] = recordHotkey_.toString(QKeySequence::PortableText);
    if (!ui->autoFileNameCheck->isChecked()) {
        s["outputFileName"] = ui->outputFileEdit->text();
    }
    s["language"] = currentLang_;

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
        ui->regionInfoLabel->setText(formatRegionInfo(
            selectedRegion_.x,
            selectedRegion_.y,
            selectedRegion_.width,
            selectedRegion_.height));
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
    if (s.contains("asioOutStartCh"))
        ui->asioOutStartChSpin->setValue(s["asioOutStartCh"].toInt());
    if (s.contains("asioOutEndCh"))
        ui->asioOutEndChSpin->setValue(s["asioOutEndCh"].toInt());
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

    if (s.contains("autoNameFormat")) {
        const QString fmt = s["autoNameFormat"].toString().trimmed();
        if (!fmt.isEmpty()) {
            autoNameFormat_ = fmt;
        }
    }

    if (s.contains("recordHotkey")) {
        const QKeySequence seq(s["recordHotkey"].toString());
        if (!seq.isEmpty()) {
            recordHotkey_ = seq;
        }
    }

    if (!ui->autoFileNameCheck->isChecked() && s.contains("outputFileName")) {
        ui->outputFileEdit->setText(s["outputFileName"].toString());
    } else {
        updateAutoFileName();
    }

    ui->outputFileEdit->setEnabled(!ui->autoFileNameCheck->isChecked());

    if (s.contains("language")) {
        currentLang_ = s["language"].toString();
        retranslateUi();
    }
}

// ============================================================================
// Settings dialog
// ============================================================================

void MainWindow::onSettingsTriggered()
{
    SettingsDialog dlg(currentLang_, this);
    dlg.setRecordHotkey(recordHotkey_);
    dlg.setDefaultOutputDir(ui->outputDirEdit->text());
    dlg.setAutoNameFormat(autoNameFormat_);

    if (dlg.exec() == QDialog::Accepted) {
        bool changed = false;
        const QString lang = dlg.selectedLanguage();
        if (lang != currentLang_) {
            currentLang_ = lang;
            retranslateUi();
            changed = true;
        }

        const QString outputDir = dlg.defaultOutputDir().trimmed();
        if (!outputDir.isEmpty() && outputDir != ui->outputDirEdit->text()) {
            ui->outputDirEdit->setText(QDir::toNativeSeparators(outputDir));
            changed = true;
        }

        const QString autoNameFormat = dlg.autoNameFormat().trimmed();
        if (!autoNameFormat.isEmpty() && autoNameFormat != autoNameFormat_) {
            autoNameFormat_ = autoNameFormat;
            updateAutoFileName();
            changed = true;
        }

        const QKeySequence hotkey = dlg.recordHotkey();
        if (!hotkey.isEmpty() && hotkey != recordHotkey_) {
            unregisterGlobalHotkey();
            recordHotkey_ = hotkey;
            registerGlobalHotkey();
            changed = true;
        }

        if (changed) {
            saveSettings();
        }
    }
}

void MainWindow::retranslateUi()
{
    bool ja = (currentLang_ == "ja");

    // Preset
    ui->presetLabel->setText(ja ? "プリセット:" : "Preset:");
    ui->presetCombo->setItemText(0, ja ? "(カスタム)" : "(Custom)");
    ui->overwritePresetBtn->setToolTip(ja ? "上書き保存" : "Overwrite preset");
    ui->saveAsPresetBtn->setToolTip(ja ? "名前をつけて保存" : "Save as new preset");
    ui->deletePresetBtn->setToolTip(ja ? "プリセットを削除" : "Delete preset");
    ui->settingsBtn->setToolTip(ja ? "設定" : "Settings");

    // Source group
    ui->sourceGroupBox->setTitle(ja ? "キャプチャソース" : "Capture Source");
    ui->captureModeLabel->setText(ja ? "モード:" : "Mode:");
    ui->captureModeCombo->setItemText(0, ja ? "スクリーン" : "Screen");
    ui->captureModeCombo->setItemText(1, ja ? "ウィンドウ" : "Window");
    ui->captureModeCombo->setItemText(2, ja ? "範囲指定" : "Region");
    ui->monitorLabel->setText(ja ? "モニター:" : "Monitor:");
    ui->windowLabel->setText(ja ? "ウィンドウ:" : "Window:");
    ui->refreshWindowsBtn->setText(ja ? "更新" : "Refresh");
    ui->regionLabel->setText(ja ? "範囲:" : "Region:");
    if (!regionSelected_) {
        ui->regionInfoLabel->setText(ja ? "未選択" : "Not selected");
    }
    ui->selectRegionBtn->setText(ja ? "範囲選択" : "Select");
    ui->autoAdjustCheck->setText(ja ? "オートアジャスト" : "Auto adjust");
    ui->autoAdjustCheck->setToolTip(ja ? "範囲選択時に近くのラインに自動でスナップします" : "Snap to nearby lines when selecting region");
    ui->captureCursorCheck->setText(ja ? "マウスカーソルをキャプチャ" : "Capture mouse cursor");
    ui->captureCursorCheck->setToolTip(ja ? "マウスカーソルをキャプチャ" : "Capture mouse cursor");
    ui->monitorCombo->setToolTip(ja ? "キャプチャするディスプレイ" : "Display to capture");
    ui->windowCombo->setToolTip(ja ? "キャプチャするウィンドウ" : "Window to capture");
    ui->refreshWindowsBtn->setToolTip(ja ? "ウィンドウ一覧を更新" : "Refresh window list");
    ui->selectRegionBtn->setToolTip(ja
        ? "画面で範囲を選択し、Enterキーで確定"
        : "Draw a capture region, then press Enter to confirm");

    // Video group
    ui->videoGroupBox->setTitle(ja ? "映像設定" : "Video Settings");
    ui->videoCodecLabel->setText(ja ? "コーデック:" : "Codec:");
    ui->containerLabel->setText(ja ? "コンテナ:" : "Container:");
    ui->fpsLabel->setText(ja ? "FPS:" : "FPS:");
    ui->videoBitrateLabel->setText(ja ? "ビットレート:" : "Bitrate:");
    ui->videoQualityLabel->setText(ja ? "品質:" : "Quality:");
    ui->realtimeEncodeCheck->setText(ja ? "リアルタイムエンコード" : "Realtime encode");
    ui->h264ProfileLabel->setText(ja ? "プロファイル:" : "Profile:");
    ui->h264LevelLabel->setText(ja ? "レベル:" : "Level:");
    ui->h264LevelCombo->setItemText(0, ja ? "自動" : "Auto");
    ui->hwEncoderCheck->setText(ja ? "ハードウェアエンコーダー (GPU)" : "Hardware encoder (GPU)");
    ui->hwEncoderCheck->setToolTip(ja ? "利用可能なら GPU エンコードを使う" : "Use GPU hardware encoder when available");
    ui->videoCodecCombo->setToolTip(ja ? "映像コーデック" : "Video codec");
    ui->containerCombo->setToolTip(ja ? "コンテナ形式" : "Container format");
    ui->fpsSpinBox->setToolTip(ja ? "フレームレート" : "Frames per second");
    ui->videoBitrateSpinBox->setToolTip(ja ? "映像ビットレート (kbps)" : "Video bitrate (kbps)");
    ui->videoQualitySlider->setToolTip(ja ? "エンコード品質 0–100" : "Encoding quality 0–100");
    ui->realtimeEncodeCheck->setToolTip(ja ? "リアルタイムエンコード (推奨)" : "Realtime encoding (recommended)");
    ui->h264ProfileCombo->setToolTip(ja ? "H.264 プロファイル" : "H.264 profile");
    ui->h264LevelCombo->setToolTip(ja ? "H.264 レベル" : "H.264 level");

    // Audio group
    ui->audioGroupBox->setTitle(ja ? "音声設定" : "Audio Settings");
    ui->outputAudioLabel->setText(ja ? "出力デバイス:" : "Output device:");
    ui->inputAudioLabel->setText(ja ? "入力デバイス:" : "Input device:");
    ui->outputAudioCombo->setItemText(0, ja ? "なし" : "None");
    ui->inputAudioCombo->setItemText(0, ja ? "なし" : "None");
    ui->asioOutChannelLabel->setText(ja ? "出力ASIOチャンネル:" : "Output ASIO ch:");
    ui->asioChannelLabel->setText(ja ? "入力ASIOチャンネル:" : "Input ASIO ch:");
    ui->audioCodecLabel->setText(ja ? "コーデック:" : "Codec:");
    ui->audioBitrateLabel->setText(ja ? "ビットレート:" : "Bitrate:");
    ui->audioSampleRateLabel->setText(ja ? "サンプリングレート:" : "Sample rate:");
    ui->audioBitDepthLabel->setText(ja ? "ビット深度:" : "Bit depth:");
    ui->vorbisQualityLabel->setText(ja ? "品質:" : "Quality:");
    ui->outputAudioCombo->setToolTip(ja ? "出力音声 (ループバック) デバイス" : "System audio (loopback) device");
    ui->inputAudioCombo->setToolTip(ja ? "入力 (マイク) デバイス" : "Microphone / input device");
    ui->audioCodecCombo->setToolTip(ja ? "音声コーデック" : "Audio codec");
    ui->audioBitrateSlider->setToolTip(ja ? "音声ビットレート (kbps)" : "Audio bitrate (kbps)");
    ui->vorbisQualitySlider->setToolTip(ja ? "エンコード品質 0–100" : "Encoding quality 0–100");

    // Output group
    ui->outputGroupBox->setTitle(ja ? "出力" : "Output");
    ui->outputDirLabel->setText(ja ? "フォルダ:" : "Folder:");
    ui->outputDirEdit->setPlaceholderText(ja ? "出力フォルダ..." : "Output folder...");
    ui->browseBtn->setText(ja ? "参照" : "Browse");
    ui->openOutputFolderBtn->setText(ja ? "開く" : "Open");
    ui->openOutputFolderBtn->setToolTip(ja
        ? "現在の出力フォルダをエクスプローラーで開く"
        : "Open the current output folder in Explorer");
    ui->outputFileLabel->setText(ja ? "ファイル名:" : "Filename:");
    ui->outputFileEdit->setPlaceholderText(ja ? "ファイル名..." : "Filename...");
    ui->autoFileNameCheck->setText(ja ? "自動" : "Auto");
    ui->autoFileNameCheck->setToolTip(ja ? "録画開始時に日時と設定からファイル名を自動生成します" : "Auto-generate filename from date/time and settings");

    // Record controls
    if (!isRecording_) {
        ui->recordBtn->setText(ja ? "録画" : "Record");
    } else {
        ui->recordBtn->setText(ja ? "停止" : "Stop");
    }
    if (!isPaused_) {
        ui->pauseBtn->setText(ja ? "一時停止" : "Pause");
    } else {
        ui->pauseBtn->setText(ja ? "再開" : "Resume");
    }
    const QString hotkeyText = recordHotkey_.toString(QKeySequence::NativeText);
    ui->recordBtn->setToolTip(
        (ja ? QStringLiteral("録画開始/停止 (Ctrl+R / %1)")
            : QStringLiteral("Start/stop recording (Ctrl+R / %1)")).arg(hotkeyText));
    ui->pauseBtn->setToolTip(ja ? "録画を一時停止/再開" : "Pause/resume recording");

    // Open folder button (status bar permanent widget)
    if (openFolderBtn_) {
        openFolderBtn_->setText(ja ? "フォルダを開く" : "Open Folder");
        openFolderBtn_->setToolTip(ja
            ? "最後に保存したファイルのフォルダを開く"
            : "Open the folder of the last saved file");
    }

    // Window title (only set static title here when not recording)
    if (!isRecording_) {
        setWindowTitle("pbRecorder");
    }
}

// ============================================================================
// Peak Meters
// ============================================================================

void MainWindow::setupPeakMeters()
{
    outputMeter_ = new PeakMeterWidget(this);
    inputMeter_ = new PeakMeterWidget(this);

    auto* audioLayout = qobject_cast<QGridLayout*>(ui->audioGroupBox->layout());
    if (audioLayout) {
        // UI layout rows:
        // row 0: output device label + combo
        // row 1: output peak meter  (added here)
        // row 2: output ASIO channel (in .ui)
        // row 3: input device label + combo (in .ui)
        // row 4: input peak meter   (added here)
        // row 5: input ASIO channel (in .ui)
        // row 6+: codec, bitrate, etc.
        audioLayout->addWidget(outputMeter_, 1, 0, 1, 3);
        audioLayout->addWidget(inputMeter_, 4, 0, 1, 3);
    }

    // Rebuild metering sessions when device selection changes
    connect(ui->outputAudioCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::rebuildMeteringSessions);
    connect(ui->inputAudioCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::rebuildMeteringSessions);

    // Build initial metering sessions
    rebuildMeteringSessions();

    // Start meter update timer (50ms = 20fps)
    connect(&meterTimer_, &QTimer::timeout, this, &MainWindow::updatePeakMeters);
    meterTimer_.start(50);
}

void MainWindow::rebuildMeteringSessions()
{
    // Output device
    int outIdx = ui->outputAudioCombo->currentIndex();
    if (outIdx > 0 && (outIdx - 1) < static_cast<int>(outputAudioDevices_.size())) {
        const auto& dev = outputAudioDevices_[outIdx - 1];
        if (dev.type == pb::AudioDeviceType::WASAPI_Render) {
            if (!outputMeteringSession_ || outputMeteringSession_->deviceId != dev.id) {
                outputMeteringSession_ = MeteringSession::create(dev.id, false);
            }
        } else {
            outputMeteringSession_.reset();
        }
    } else {
        outputMeteringSession_.reset();
    }

    // Input device
    int inIdx = ui->inputAudioCombo->currentIndex();
    if (inIdx > 0 && (inIdx - 1) < static_cast<int>(inputAudioDevices_.size())) {
        const auto& dev = inputAudioDevices_[inIdx - 1];
        if (dev.type == pb::AudioDeviceType::WASAPI_Capture) {
            if (!inputMeteringSession_ || inputMeteringSession_->deviceId != dev.id) {
                inputMeteringSession_ = MeteringSession::create(dev.id, true);
            }
        } else {
            inputMeteringSession_.reset();
        }
    } else {
        inputMeteringSession_.reset();
    }
}

void MainWindow::releaseMeteringSessions()
{
    outputMeteringSession_.reset();
    inputMeteringSession_.reset();
}

void MainWindow::updatePeakMeters()
{
    // Output device meter
    int outIdx = ui->outputAudioCombo->currentIndex();
    if (outIdx > 0 && (outIdx - 1) < static_cast<int>(outputAudioDevices_.size())) {
        const auto& dev = outputAudioDevices_[outIdx - 1];
        if (dev.type == pb::AudioDeviceType::WASAPI_Render) {
            if (outputMeteringSession_) {
                outputMeter_->setLevel(outputMeteringSession_->getPeak());
            } else {
                outputMeter_->setLevel(0.0f);
            }
        } else if (dev.type == pb::AudioDeviceType::ASIO ||
                   dev.type == pb::AudioDeviceType::ASIO_Output) {
            outputMeter_->setLevel(pb::AsioCapture::getInstancePeakLevel());
        } else {
            outputMeter_->setLevel(0.0f);
        }
    } else {
        outputMeter_->setLevel(0.0f);
    }

    // Input device meter
    int inIdx = ui->inputAudioCombo->currentIndex();
    if (inIdx > 0 && (inIdx - 1) < static_cast<int>(inputAudioDevices_.size())) {
        const auto& dev = inputAudioDevices_[inIdx - 1];
        if (dev.type == pb::AudioDeviceType::WASAPI_Capture) {
            if (inputMeteringSession_) {
                inputMeter_->setLevel(inputMeteringSession_->getPeak());
            } else {
                inputMeter_->setLevel(0.0f);
            }
        } else if (dev.type == pb::AudioDeviceType::ASIO) {
            inputMeter_->setLevel(pb::AsioCapture::getInstancePeakLevel());
        } else {
            inputMeter_->setLevel(0.0f);
        }
    } else {
        inputMeter_->setLevel(0.0f);
    }
}
