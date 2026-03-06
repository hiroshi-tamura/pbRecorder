#pragma once

#include <QWidget>
#include <QRect>
#include <QPoint>
#include <QLabel>

/// Full-screen transparent overlay for selecting a rectangular capture region.
/// Covers the entire virtual desktop (all monitors).
/// Click and drag to draw a selection rectangle.
/// Press Enter to confirm, Escape to cancel.
class RegionSelectorWidget : public QWidget
{
    Q_OBJECT

public:
    explicit RegionSelectorWidget(QWidget *parent = nullptr);
    ~RegionSelectorWidget() override;

signals:
    /// Emitted when the user confirms the selection (Enter key).
    /// Coordinates are in physical (unscaled) pixels relative to the virtual desktop.
    void regionSelected(int x, int y, int width, int height);

    /// Emitted when the user cancels selection (Escape key).
    void selectionCancelled();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    /// Compute the virtual desktop geometry covering all screens.
    QRect virtualDesktopGeometry() const;

    /// Convert widget-local coordinates to physical desktop pixels (DPI-aware).
    QRect toPhysicalPixels(const QRect &logicalRect) const;

    /// Normalize the rect so width/height are positive.
    QRect normalizedSelection() const;

    /// Update the dimension label position and text.
    void updateDimensionLabel();

    bool selecting_ = false;
    bool hasSelection_ = false;
    QPoint startPos_;
    QPoint currentPos_;

    QLabel *dimensionLabel_ = nullptr;

    // Device pixel ratio for DPI conversion
    qreal devicePixelRatio_ = 1.0;
};
