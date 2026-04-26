#include "SettingsDialog.h"

#include <QWidget>
#include <QHBoxLayout>
#include <QFileDialog>

SettingsDialog::SettingsDialog(const QString& currentLang, QWidget *parent)
    : QDialog(parent)
    , currentLang_(currentLang)
{
    const bool ja = (currentLang_ == "ja");

    setWindowTitle(ja ? "設定" : "Settings");
    setMinimumWidth(380);

    auto *layout = new QVBoxLayout(this);

    auto *tabs = new QTabWidget(this);

    // ---------- General tab ----------
    auto *generalTab = new QWidget(tabs);
    {
        auto *form = new QFormLayout(generalTab);

        langCombo_ = new QComboBox(generalTab);
        langCombo_->addItem("日本語", "ja");
        langCombo_->addItem("English", "en");

        for (int i = 0; i < langCombo_->count(); ++i) {
            if (langCombo_->itemData(i).toString() == currentLang_) {
                langCombo_->setCurrentIndex(i);
                break;
            }
        }

        const QString langLabel = ja ? "言語:" : "Language:";
        form->addRow(new QLabel(langLabel, generalTab), langCombo_);
    }
    tabs->addTab(generalTab, ja ? "一般" : "General");

    // ---------- Hotkeys tab ----------
    auto *hotkeysTab = new QWidget(tabs);
    {
        auto *form = new QFormLayout(hotkeysTab);

        recordHotkeyEdit_ = new QKeySequenceEdit(hotkeysTab);
        recordHotkeyEdit_->setKeySequence(QKeySequence("Ctrl+Shift+R"));

        const QString hotkeyLabel = ja ? "録画開始/停止:" : "Start/Stop Recording:";
        form->addRow(new QLabel(hotkeyLabel, hotkeysTab), recordHotkeyEdit_);
    }
    tabs->addTab(hotkeysTab, ja ? "ホットキー" : "Hotkeys");

    // ---------- Output tab ----------
    auto *outputTab = new QWidget(tabs);
    {
        auto *form = new QFormLayout(outputTab);

        // Default output directory: QLineEdit + 参照... button
        auto *dirRow = new QWidget(outputTab);
        auto *dirRowLayout = new QHBoxLayout(dirRow);
        dirRowLayout->setContentsMargins(0, 0, 0, 0);

        outputDirEdit_ = new QLineEdit(dirRow);
        outputDirBrowseButton_ = new QPushButton(ja ? "参照…" : "Browse…", dirRow);

        dirRowLayout->addWidget(outputDirEdit_, 1);
        dirRowLayout->addWidget(outputDirBrowseButton_, 0);

        connect(outputDirBrowseButton_, &QPushButton::clicked, this, [this, ja]() {
            const QString caption = ja ? "既定保存先フォルダを選択" : "Select default output folder";
            const QString start = outputDirEdit_->text();
            const QString chosen = QFileDialog::getExistingDirectory(this, caption, start);
            if (!chosen.isEmpty()) {
                outputDirEdit_->setText(chosen);
            }
        });

        const QString dirLabel = ja ? "既定保存先フォルダ:" : "Default output folder:";
        form->addRow(new QLabel(dirLabel, outputTab), dirRow);

        // Auto-naming format
        autoNameFormatEdit_ = new QLineEdit(outputTab);
        autoNameFormatEdit_->setText("yyyy-MM-dd_HH-mm-ss");

        const QString fmtLabel = ja ? "自動命名フォーマット:" : "Auto-naming format:";
        form->addRow(new QLabel(fmtLabel, outputTab), autoNameFormatEdit_);
    }
    tabs->addTab(outputTab, ja ? "出力" : "Output");

    layout->addWidget(tabs);

    // ---------- Buttons ----------
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    if (ja) {
        if (auto *cancelBtn = buttons->button(QDialogButtonBox::Cancel)) {
            cancelBtn->setText("キャンセル");
        }
        // OK ボタンはそのまま「OK」
    }
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

QString SettingsDialog::selectedLanguage() const
{
    return langCombo_->currentData().toString();
}

QKeySequence SettingsDialog::recordHotkey() const
{
    return recordHotkeyEdit_->keySequence();
}

void SettingsDialog::setRecordHotkey(const QKeySequence& seq)
{
    recordHotkeyEdit_->setKeySequence(seq);
}

QString SettingsDialog::defaultOutputDir() const
{
    return outputDirEdit_->text();
}

void SettingsDialog::setDefaultOutputDir(const QString& dir)
{
    outputDirEdit_->setText(dir);
}

QString SettingsDialog::autoNameFormat() const
{
    return autoNameFormatEdit_->text();
}

void SettingsDialog::setAutoNameFormat(const QString& fmt)
{
    autoNameFormatEdit_->setText(fmt);
}
