#include "RegionSelectorWidget.h"

#include <QPainter>
#include <QPen>
#include <QBrush>
#include <QColor>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QShowEvent>
#include <QScreen>
#include <QGuiApplication>
#include <QCursor>

// ============================================================================
// Construction / Destruction
// ============================================================================

RegionSelectorWidget::RegionSelectorWidget(QWidget *parent)
    : QWidget(parent)
{
    // Window flags: frameless, always on top, tool window (no taskbar icon)
    setWindowFlags(Qt::FramelessWindowHint
                   | Qt::WindowStaysOnTopHint
                   | Qt::Tool);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose);

    // Crosshair cursor
    setCursor(Qt::CrossCursor);

    // Dimension label (child widget, floating near selection)
    dimensionLabel_ = new QLabel(this);
    dimensionLabel_->setStyleSheet(
        "QLabel {"
        "  background-color: rgba(0, 0, 0, 180);"
        "  color: white;"
        "  padding: 4px 8px;"
        "  border-radius: 3px;"
        "  font-size: 12px;"
        "  font-family: Consolas, monospace;"
        "}");
    dimensionLabel_->hide();

    // Cover the entire virtual desktop
    QRect vd = virtualDesktopGeometry();
    setGeometry(vd);
}

RegionSelectorWidget::~RegionSelectorWidget() = default;

// ============================================================================
// Show event
// ============================================================================

void RegionSelectorWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);

    // Ensure we cover the entire virtual desktop
    QRect vd = virtualDesktopGeometry();
    setGeometry(vd);

    // Cache device pixel ratio for DPI conversion
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen) {
        devicePixelRatio_ = screen->devicePixelRatio();
    }

    // Grab keyboard focus
    activateWindow();
    setFocus();
}

// ============================================================================
// Paint
// ============================================================================

void RegionSelectorWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);

    // Semi-transparent dark overlay covering everything
    painter.fillRect(rect(), QColor(0, 0, 0, 100));

    if (selecting_ || hasSelection_) {
        QRect sel = normalizedSelection();

        // Clear the selected region (make it transparent / "cut out")
        painter.setCompositionMode(QPainter::CompositionMode_Clear);
        painter.fillRect(sel, Qt::transparent);

        // Switch back to normal composition
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

        // Draw selection border
        QPen pen(QColor(50, 130, 255), 2);
        pen.setStyle(Qt::SolidLine);
        painter.setPen(pen);
        painter.setBrush(QColor(50, 130, 255, 40));
        painter.drawRect(sel);

        // Draw corner handles
        const int handleSize = 6;
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(50, 130, 255));

        QPoint corners[] = {
            sel.topLeft(), sel.topRight(),
            sel.bottomLeft(), sel.bottomRight()
        };
        for (const auto &corner : corners) {
            painter.drawRect(corner.x() - handleSize / 2,
                             corner.y() - handleSize / 2,
                             handleSize, handleSize);
        }
    }
}

// ============================================================================
// Mouse events
// ============================================================================

void RegionSelectorWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        selecting_ = true;
        hasSelection_ = false;
        startPos_ = event->pos();
        currentPos_ = event->pos();
        dimensionLabel_->hide();
        update();
    }
}

void RegionSelectorWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (selecting_) {
        currentPos_ = event->pos();
        updateDimensionLabel();
        update();
    }
}

void RegionSelectorWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && selecting_) {
        selecting_ = false;
        currentPos_ = event->pos();

        QRect sel = normalizedSelection();
        if (sel.width() > 5 && sel.height() > 5) {
            hasSelection_ = true;
            updateDimensionLabel();
        } else {
            hasSelection_ = false;
            dimensionLabel_->hide();
        }
        update();
    }
}

// ============================================================================
// Key events
// ============================================================================

void RegionSelectorWidget::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (hasSelection_) {
            QRect sel = normalizedSelection();
            QRect phys = toPhysicalPixels(sel);
            emit regionSelected(phys.x(), phys.y(), phys.width(), phys.height());
            close();
            return;
        }
    }

    if (event->key() == Qt::Key_Escape) {
        emit selectionCancelled();
        close();
        return;
    }

    QWidget::keyPressEvent(event);
}

// ============================================================================
// Helpers
// ============================================================================

QRect RegionSelectorWidget::virtualDesktopGeometry() const
{
    QRect combined;
    const auto screens = QGuiApplication::screens();
    for (const QScreen *screen : screens) {
        combined = combined.united(screen->geometry());
    }
    return combined;
}

QRect RegionSelectorWidget::toPhysicalPixels(const QRect &logicalRect) const
{
    // The widget covers the virtual desktop in logical coordinates.
    // We need to find which screen the center of the selection falls on
    // and use its DPR for the conversion.
    QPoint center = logicalRect.center();

    // Map to global coordinates (widget position is at virtual desktop origin)
    QPoint globalCenter = mapToGlobal(center);

    qreal dpr = devicePixelRatio_;

    // Try to find the exact screen for better DPI accuracy
    QScreen *targetScreen = QGuiApplication::screenAt(globalCenter);
    if (targetScreen) {
        dpr = targetScreen->devicePixelRatio();
    }

    // Convert the rect: widget coordinates are already in virtual desktop space
    // because the widget covers the virtual desktop starting from its origin.
    QPoint globalTopLeft = mapToGlobal(logicalRect.topLeft());

    return QRect(
        static_cast<int>(globalTopLeft.x() * dpr),
        static_cast<int>(globalTopLeft.y() * dpr),
        static_cast<int>(logicalRect.width() * dpr),
        static_cast<int>(logicalRect.height() * dpr)
    );
}

QRect RegionSelectorWidget::normalizedSelection() const
{
    return QRect(startPos_, currentPos_).normalized();
}

void RegionSelectorWidget::updateDimensionLabel()
{
    QRect sel = normalizedSelection();
    if (sel.width() < 2 && sel.height() < 2) {
        dimensionLabel_->hide();
        return;
    }

    // Show physical pixel dimensions
    QRect phys = toPhysicalPixels(sel);
    QString text = QString("%1 x %2").arg(phys.width()).arg(phys.height());
    if (hasSelection_) {
        text += QString::fromUtf8("\nEnterキーで確定 / Escでキャンセル");
    }
    dimensionLabel_->setText(text);
    dimensionLabel_->adjustSize();

    // Position the label just below the bottom-right corner of selection
    int labelX = sel.right() - dimensionLabel_->width();
    int labelY = sel.bottom() + 8;

    // Clamp to widget bounds
    if (labelX < 0) labelX = 0;
    if (labelY + dimensionLabel_->height() > height()) {
        labelY = sel.top() - dimensionLabel_->height() - 8;
    }
    if (labelY < 0) labelY = 0;

    dimensionLabel_->move(labelX, labelY);
    dimensionLabel_->show();
}
