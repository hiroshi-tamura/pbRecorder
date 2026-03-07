#pragma once

#include <QMainWindow>
#include <QTimer>
#include <QElapsedTimer>
#include <QJsonObject>
#include <memory>
#include <vector>

#include "core/Types.h"

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class RegionSelectorWidget;

namespace pb {
class IRecordingPipeline;
class ICaptureSource;
class IAudioSource;
class MonitorEnumerator;
class WindowEnumerator;
class RecordingSession;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onCaptureModeChanged(int index);
    void onVideoCodecChanged(int index);
    void onContainerChanged(int index);
    void onRefreshWindows();
    void onSelectRegion();
    void onBrowse();
    void onRecord();
    void onPause();
    void onUpdateTimer();
    void onVideoBitrateSliderChanged(int value);
    void onVideoBitrateSpinBoxChanged(int value);
    void onAudioBitrateSliderChanged(int value);
    void onAudioBitrateSpinBoxChanged(int value);
    void onRecordAudioToggled(bool checked);
    void onAudioCodecChanged(int index);
    void onSavePreset();
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
    void updateCaptureWidgetVisibility(int mode);
    void updateOutputExtension();
    void updateAutoFileName();
    QString generateAutoFileName() const;
    QString getOutputFilePath() const;
    void updateAudioCodecWidgets();

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

    std::unique_ptr<Ui::MainWindow> ui;

    // Enumerators
    std::unique_ptr<pb::MonitorEnumerator> monitorEnum_;
    std::unique_ptr<pb::WindowEnumerator> windowEnum_;

    // Recording session
    std::unique_ptr<pb::RecordingSession> session_;

    // Data caches
    std::vector<pb::MonitorInfo> monitors_;
    std::vector<pb::WindowInfo> windows_;
    std::vector<pb::AudioDeviceInfo> audioDevices_;
    std::vector<pb::AudioDeviceInfo> outputAudioDevices_; // render (speakers)
    std::vector<pb::AudioDeviceInfo> inputAudioDevices_;  // capture (mics) + ASIO

    // Region selection
    pb::RegionRect selectedRegion_{};
    bool regionSelected_ = false;

    // Recording state
    bool isRecording_ = false;
    bool isPaused_ = false;
    bool errorShown_ = false;
    QTimer updateTimer_;
    QElapsedTimer recordingElapsed_;
    int64_t pausedAccumMs_ = 0;
    int64_t pauseStartMs_ = 0;
};
