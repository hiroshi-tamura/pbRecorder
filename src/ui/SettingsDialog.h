#pragma once

#include <QDialog>
#include <QSettings>
#include <memory>

QT_BEGIN_NAMESPACE
namespace Ui { class SettingsDialog; }
QT_END_NAMESPACE

class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget *parent = nullptr);
    ~SettingsDialog() override;

    // Accessors for settings values
    QString h264Profile() const;
    QString h264Level() const;
    bool useHardwareEncoder() const;
    int sampleRate() const;
    int bitDepth() const;
    QString defaultOutputFolder() const;
    bool captureCursor() const;

    // Load / save to persistent storage
    void loadSettings();
    void saveSettings();

private slots:
    void onBrowseFolder();
    void onApply();
    void onAccepted();

private:
    void setupConnections();
    void applySettingsToUi();

    std::unique_ptr<Ui::SettingsDialog> ui;
    QSettings settings_;

    // Defaults
    static constexpr int kDefaultSampleRateIndex = 1;  // 48000
    static constexpr int kDefaultBitDepthIndex = 0;     // 16-bit
    static constexpr int kDefaultProfileIndex = 2;      // High
    static constexpr int kDefaultLevelIndex = 0;        // Auto
};
