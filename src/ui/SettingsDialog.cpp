#include "SettingsDialog.h"
#include "ui_SettingsDialog.h"

#include <QFileDialog>
#include <QStandardPaths>

// ============================================================================
// Construction / Destruction
// ============================================================================

SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent)
    , ui(std::make_unique<Ui::SettingsDialog>())
    , settings_("pbRecorder", "pbRecorder")
{
    ui->setupUi(this);
    setupConnections();
    loadSettings();
}

SettingsDialog::~SettingsDialog() = default;

// ============================================================================
// Connections
// ============================================================================

void SettingsDialog::setupConnections()
{
    connect(ui->browseFolderBtn, &QPushButton::clicked,
            this, &SettingsDialog::onBrowseFolder);

    connect(ui->buttonBox, &QDialogButtonBox::accepted,
            this, &SettingsDialog::onAccepted);
    connect(ui->buttonBox, &QDialogButtonBox::rejected,
            this, &QDialog::reject);

    // Apply button
    auto *applyBtn = ui->buttonBox->button(QDialogButtonBox::Apply);
    if (applyBtn) {
        connect(applyBtn, &QPushButton::clicked,
                this, &SettingsDialog::onApply);
    }
}

// ============================================================================
// Accessors
// ============================================================================

QString SettingsDialog::h264Profile() const
{
    return ui->h264ProfileCombo->currentText();
}

QString SettingsDialog::h264Level() const
{
    return ui->h264LevelCombo->currentText();
}

bool SettingsDialog::useHardwareEncoder() const
{
    return ui->hwEncoderCheck->isChecked();
}

int SettingsDialog::sampleRate() const
{
    static const int rates[] = {44100, 48000, 96000};
    int idx = ui->sampleRateCombo->currentIndex();
    if (idx >= 0 && idx < 3) return rates[idx];
    return 48000;
}

int SettingsDialog::bitDepth() const
{
    static const int depths[] = {16, 24, 32};
    int idx = ui->bitDepthCombo->currentIndex();
    if (idx >= 0 && idx < 3) return depths[idx];
    return 16;
}

QString SettingsDialog::defaultOutputFolder() const
{
    return ui->defaultFolderEdit->text();
}

bool SettingsDialog::captureCursor() const
{
    return ui->captureCursorCheck->isChecked();
}

// ============================================================================
// Load / Save
// ============================================================================

void SettingsDialog::loadSettings()
{
    ui->h264ProfileCombo->setCurrentIndex(
        settings_.value("video/h264Profile", kDefaultProfileIndex).toInt());
    ui->h264LevelCombo->setCurrentIndex(
        settings_.value("video/h264Level", kDefaultLevelIndex).toInt());
    ui->hwEncoderCheck->setChecked(
        settings_.value("video/hwEncoder", true).toBool());

    ui->sampleRateCombo->setCurrentIndex(
        settings_.value("audio/sampleRate", kDefaultSampleRateIndex).toInt());
    ui->bitDepthCombo->setCurrentIndex(
        settings_.value("audio/bitDepth", kDefaultBitDepthIndex).toInt());

    QString defaultFolder = settings_.value(
        "general/outputFolder",
        QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)).toString();
    ui->defaultFolderEdit->setText(defaultFolder);

    ui->captureCursorCheck->setChecked(
        settings_.value("general/captureCursor", true).toBool());
}

void SettingsDialog::saveSettings()
{
    settings_.setValue("video/h264Profile", ui->h264ProfileCombo->currentIndex());
    settings_.setValue("video/h264Level", ui->h264LevelCombo->currentIndex());
    settings_.setValue("video/hwEncoder", ui->hwEncoderCheck->isChecked());

    settings_.setValue("audio/sampleRate", ui->sampleRateCombo->currentIndex());
    settings_.setValue("audio/bitDepth", ui->bitDepthCombo->currentIndex());

    settings_.setValue("general/outputFolder", ui->defaultFolderEdit->text());
    settings_.setValue("general/captureCursor", ui->captureCursorCheck->isChecked());

    settings_.sync();
}

// ============================================================================
// Slots
// ============================================================================

void SettingsDialog::onBrowseFolder()
{
    QString dir = QFileDialog::getExistingDirectory(
        this, tr("既定の出力フォルダを選択"),
        ui->defaultFolderEdit->text());
    if (!dir.isEmpty()) {
        ui->defaultFolderEdit->setText(dir);
    }
}

void SettingsDialog::onApply()
{
    saveSettings();
}

void SettingsDialog::onAccepted()
{
    saveSettings();
    accept();
}
