#include "SettingsDialog.h"

SettingsDialog::SettingsDialog(const QString& currentLang, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(currentLang == "en" ? "Settings" : "設定");
    setMinimumWidth(300);

    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;

    langCombo_ = new QComboBox;
    langCombo_->addItem("日本語", "ja");
    langCombo_->addItem("English", "en");

    // Set current
    for (int i = 0; i < langCombo_->count(); ++i) {
        if (langCombo_->itemData(i).toString() == currentLang) {
            langCombo_->setCurrentIndex(i);
            break;
        }
    }

    QString label = currentLang == "en" ? "Language:" : "言語:";
    form->addRow(new QLabel(label), langCombo_);
    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

QString SettingsDialog::selectedLanguage() const
{
    return langCombo_->currentData().toString();
}
