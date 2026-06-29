#pragma once

#include <QMainWindow>
#include <QTimer>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QKeySequence>
#include <memory>
#include <vector>
#include <atomic>
#include <optional>

#include "core/Types.h"

class PeakMeterWidget;
class QPushButton;
class QLabel;
class QWidget;

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class RegionSelectorWidget;
class UiElementSelectorWidget;

namespace pb {
class IRecordingPipeline;
class ICaptureSource;
class IAudioSource;
class MonitorEnumerator;
class WindowEnumerator;
class RecordingSession;
class UiAutomationHelper;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;

private slots:
    void onCaptureModeChanged(int index);
    void onVideoCodecChanged(int index);
    void onContainerChanged(int index);
    void onRefreshWindows();
    void onSelectRegion();
    void onSelectUiElement();
    void onBrowse();
    void onOpenOutputFolder();
    void onRecord();
    void onPause();
    void onUpdateTimer();
    void onVideoBitrateSliderChanged(int value);
    void onVideoBitrateSpinBoxChanged(int value);
    void onAudioBitrateSliderChanged(int value);
    void onAudioBitrateSpinBoxChanged(int value);
    void onRecordAudioToggled(bool checked);
    void onAudioCodecChanged(int index);
    void onOverwritePreset();
    void onSaveAsPreset();
    void onDeletePreset();
    void onPresetChanged(int index);
    void onSettingsTriggered();

private:
    void setupConnections();
    void populateMonitors();
    void populateWindows();
    void populateAudioDevices();
    void updateContainerCombo();
    void updateAudioCodecCombo();
    void validateAudioCodec();
    void enforceSingleAudioSource(QObject *changedCombo);
    QString formatRegionInfo(int x, int y, int w, int h) const;
    QString formatUiElementInfo(const pb::UiElementTarget& target) const;
    void updateCaptureWidgetVisibility(int mode);
    void updateRecordButtonGuard();
    void updateProcessLoopbackItemState();
    void updateAsioChannelVisibility();
    void lockWindowSize();
    void refitWindow();
    void registerGlobalHotkey();
    void unregisterGlobalHotkey();
    void updateOutputExtension();
    void updateAutoFileName();
    QString generateAutoFileName() const;
    QString getOutputFilePath() const;
    void updateAudioCodecWidgets();
    void updateCapturePreview();
    std::optional<pb::RegionRect> currentCapturePreviewRect();

    QString settingsFilePath() const;
    QJsonObject loadJson() const;
    void saveJson(const QJsonObject& root) const;
    void loadPresets();
    void applyPreset(const QString& name);
    void saveCurrentAsPreset(const QString& name);
    void saveSettings();
    void loadSettings();

    pb::RecordingConfig buildRecordingConfig() const;
    void retranslateUi();
    void setRecordingState(bool recording);
    void setPausedState(bool paused);

    QString formatDuration(int64_t ms) const;
    QString formatFileSize(int64_t bytes) const;


    QString currentLang_ = "en";
    QString autoNameFormat_ = "yyyy-MM-dd_HH-mm-ss";

    // Window is made non-resizable/snug once on first show.
    bool sizeLocked_ = false;
    bool refitEnabled_ = false;

    // Last saved file path (for "Open Folder" button)
    QString lastSavedPath_;
    QPushButton* openFolderBtn_ = nullptr;

    // Global hotkey state
    QKeySequence recordHotkey_ = QKeySequence("Ctrl+Shift+R");
    bool hotkeyRegistered_ = false;

    std::unique_ptr<Ui::MainWindow> ui;

    // Enumerators
    std::unique_ptr<pb::MonitorEnumerator> monitorEnum_;
    std::unique_ptr<pb::WindowEnumerator> windowEnum_;

    // Recording session
    std::unique_ptr<pb::RecordingSession> session_;
    std::unique_ptr<pb::UiAutomationHelper> previewUiAutomation_;

    // Data caches
    std::vector<pb::MonitorInfo> monitors_;
    std::vector<pb::WindowInfo> windows_;
    std::vector<pb::AudioDeviceInfo> audioDevices_;
    std::vector<pb::AudioDeviceInfo> outputAudioDevices_; // render (speakers) + process loopback
    std::vector<pb::AudioDeviceInfo> inputAudioDevices_;  // capture (mics) + ASIO
    int processLoopbackComboIndex_ = -1; // output combo index of the per-app loopback entry

    // Region selection
    pb::RegionRect selectedRegion_{};
    bool regionSelected_ = false;
    pb::UiElementTarget selectedUiElement_{};
    bool uiElementSelected_ = false;
    bool audioCodecCompatible_ = true;

    // Recording state
    bool isRecording_ = false;
    bool isPaused_ = false;
    bool errorShown_ = false;
    bool recordingUsesOutputAudio_ = false;
    bool recordingUsesInputAudio_ = false;
    std::atomic<float> recordingAudioLevel_{0.0f};
    QTimer updateTimer_;
    QTimer capturePreviewTimer_;
    QElapsedTimer recordingElapsed_;
    int64_t pausedAccumMs_ = 0;
    int64_t pauseStartMs_ = 0;

    // Peak meters
    PeakMeterWidget* outputMeter_ = nullptr;
    PeakMeterWidget* inputMeter_ = nullptr;
    QTimer meterTimer_;
    void setupPeakMeters();
    void updatePeakMeters();

    // Metering sessions (keep WASAPI devices active for peak meters)
    struct MeteringSession;
    std::unique_ptr<MeteringSession> outputMeteringSession_;
    std::unique_ptr<MeteringSession> inputMeteringSession_;
    void rebuildMeteringSessions();
    void releaseMeteringSessions();
};
