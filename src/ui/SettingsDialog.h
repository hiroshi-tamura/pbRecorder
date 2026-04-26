#pragma once

#include <QDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QTabWidget>
#include <QKeySequenceEdit>
#include <QKeySequence>
#include <QLineEdit>
#include <QPushButton>
#include <QString>

class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SettingsDialog(const QString& currentLang, QWidget *parent = nullptr);

    // General
    QString selectedLanguage() const;

    // Hotkeys
    QKeySequence recordHotkey() const;
    void setRecordHotkey(const QKeySequence& seq);

    // Output
    QString defaultOutputDir() const;
    void setDefaultOutputDir(const QString& dir);
    QString autoNameFormat() const;
    void setAutoNameFormat(const QString& fmt);

private:
    QString currentLang_;

    // General
    QComboBox *langCombo_ = nullptr;

    // Hotkeys
    QKeySequenceEdit *recordHotkeyEdit_ = nullptr;

    // Output
    QLineEdit *outputDirEdit_ = nullptr;
    QPushButton *outputDirBrowseButton_ = nullptr;
    QLineEdit *autoNameFormatEdit_ = nullptr;
};
