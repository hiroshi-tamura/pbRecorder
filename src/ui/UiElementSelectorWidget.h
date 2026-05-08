#pragma once

#include "core/Types.h"
#include "core/UiAutomationHelper.h"

#include <QLabel>
#include <QRect>
#include <QTimer>
#include <QWidget>

#include <windows.h>

class UiElementSelectorWidget : public QWidget
{
    Q_OBJECT

public:
    explicit UiElementSelectorWidget(QWidget *parent = nullptr);
    ~UiElementSelectorWidget() override;

    void setLanguage(const QString& lang) { currentLang_ = lang; }

signals:
    void uiElementSelected(const pb::UiElementTarget& target);
    void selectionCancelled();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    QRect virtualDesktopGeometry() const;
    QRect toLogicalRect(const pb::RegionRect& rect) const;
    void refreshCandidate();
    void updateInfoLabel();
    bool confirmCurrent();

    pb::UiAutomationHelper uiAutomation_;
    std::vector<pb::UiElementPreview> candidates_;
    int candidateIndex_ = 0;
    pb::UiElementPreview currentPreview_;
    POINT currentPoint_ = {};
    bool hasCandidate_ = false;
    QLabel* infoLabel_ = nullptr;
    QTimer refreshTimer_;
    QString currentLang_ = "en";
};
