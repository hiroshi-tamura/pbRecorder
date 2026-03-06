#pragma once

#include <QWidget>
#include <QRect>
#include <QPoint>
#include <QLabel>
#include <QImage>
#include <vector>

class RegionSelectorWidget : public QWidget
{
    Q_OBJECT

public:
    explicit RegionSelectorWidget(QWidget *parent = nullptr);
    ~RegionSelectorWidget() override;

    void setAutoAdjust(bool enabled) { autoAdjust_ = enabled; }

signals:
    void regionSelected(int x, int y, int width, int height);
    void selectionCancelled();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    QRect virtualDesktopGeometry() const;
    QRect toPhysicalPixels(const QRect &logicalRect) const;
    QRect normalizedSelection() const;
    void updateDimensionLabel();

    // Auto-adjust (edge snapping)
    void captureScreenImage();
    void detectEdges();
    int snapToEdge(int pos, bool horizontal) const;
    QRect snapSelection(const QRect &sel) const;

    enum Edge { EdgeNone, EdgeLeft, EdgeRight, EdgeTop, EdgeBottom };
    Edge hitTestEdge(const QPoint &pos) const;

    bool selecting_ = false;
    bool hasSelection_ = false;
    QPoint startPos_;
    QPoint currentPos_;

    QLabel *dimensionLabel_ = nullptr;
    qreal devicePixelRatio_ = 1.0;

    // Auto-adjust state
    bool autoAdjust_ = false;
    QImage screenCapture_;
    std::vector<int> horizontalEdges_; // Y positions of horizontal lines
    std::vector<int> verticalEdges_;   // X positions of vertical lines
    static constexpr int SNAP_DISTANCE = 12;

    // Edge dragging
    Edge hoveredEdge_ = EdgeNone;
    Edge draggingEdge_ = EdgeNone;
    QRect adjustedSelection_;
};
