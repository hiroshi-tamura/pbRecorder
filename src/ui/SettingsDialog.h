#pragma once

#include <QDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QLabel>

class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SettingsDialog(const QString& currentLang, QWidget *parent = nullptr);
    QString selectedLanguage() const;
private:
    QComboBox *langCombo_;
};
