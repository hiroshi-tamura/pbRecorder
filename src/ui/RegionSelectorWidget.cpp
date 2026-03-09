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
#include <algorithm>
#include <cmath>

// ============================================================================
// Construction / Destruction
// ============================================================================

RegionSelectorWidget::RegionSelectorWidget(QWidget *parent)
    : QWidget(parent)
{
    setWindowFlags(Qt::FramelessWindowHint
                   | Qt::WindowStaysOnTopHint
                   | Qt::Tool);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose);

    setCursor(Qt::CrossCursor);
    setMouseTracking(true);

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

    QRect vd = virtualDesktopGeometry();
    setGeometry(vd);

    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen) {
        devicePixelRatio_ = screen->devicePixelRatio();
    }

    if (autoAdjust_) {
        captureScreenImage();
        detectEdges();
    }

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

    // Semi-transparent dark overlay
    painter.fillRect(rect(), QColor(0, 0, 0, 100));

    if (selecting_ || hasSelection_) {
        QRect sel = normalizedSelection();

        // Clear selected region
        painter.setCompositionMode(QPainter::CompositionMode_Clear);
        painter.fillRect(sel, Qt::transparent);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

        // Selection border
        QPen pen(QColor(50, 130, 255), 2);
        pen.setStyle(Qt::SolidLine);
        painter.setPen(pen);
        painter.setBrush(QColor(50, 130, 255, 40));
        painter.drawRect(sel);

        // Corner handles
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

        // Highlight hovered edge
        if (hasSelection_ && !selecting_ && hoveredEdge_ != EdgeNone) {
            QPen edgePen(QColor(255, 200, 50), 3);
            painter.setPen(edgePen);
            switch (hoveredEdge_) {
            case EdgeLeft:
                painter.drawLine(sel.topLeft(), sel.bottomLeft());
                break;
            case EdgeRight:
                painter.drawLine(sel.topRight(), sel.bottomRight());
                break;
            case EdgeTop:
                painter.drawLine(sel.topLeft(), sel.topRight());
                break;
            case EdgeBottom:
                painter.drawLine(sel.bottomLeft(), sel.bottomRight());
                break;
            default:
                break;
            }
        }
    }
}

// ============================================================================
// Mouse events
// ============================================================================

void RegionSelectorWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        // Check if clicking on an edge to drag it
        if (hasSelection_ && !selecting_) {
            Edge edge = hitTestEdge(event->pos());
            if (edge != EdgeNone) {
                draggingEdge_ = edge;
                adjustedSelection_ = normalizedSelection();
                return;
            }
        }

        // Track physical cursor position to determine the correct monitor
        POINT physPt;
        GetCursorPos(&physPt);
        selectionMonitor_ = MonitorFromPoint(physPt, MONITOR_DEFAULTTONEAREST);

        selecting_ = true;
        hasSelection_ = false;
        draggingEdge_ = EdgeNone;
        hoveredEdge_ = EdgeNone;
        startPos_ = event->pos();
        currentPos_ = event->pos();
        dimensionLabel_->hide();
        update();
    }
}

void RegionSelectorWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (draggingEdge_ != EdgeNone) {
        QRect sel = adjustedSelection_;
        int pos = 0;
        switch (draggingEdge_) {
        case EdgeLeft:
            pos = event->pos().x();
            if (autoAdjust_) pos = snapToEdge(pos, false);
            sel.setLeft(pos);
            break;
        case EdgeRight:
            pos = event->pos().x();
            if (autoAdjust_) pos = snapToEdge(pos, false);
            sel.setRight(pos);
            break;
        case EdgeTop:
            pos = event->pos().y();
            if (autoAdjust_) pos = snapToEdge(pos, true);
            sel.setTop(pos);
            break;
        case EdgeBottom:
            pos = event->pos().y();
            if (autoAdjust_) pos = snapToEdge(pos, true);
            sel.setBottom(pos);
            break;
        default:
            break;
        }
        startPos_ = sel.topLeft();
        currentPos_ = sel.bottomRight();
        updateDimensionLabel();
        update();
        return;
    }

    if (selecting_) {
        currentPos_ = event->pos();
        updateDimensionLabel();
        update();
    } else if (hasSelection_) {
        // Update hovered edge for highlight
        Edge newHover = hitTestEdge(event->pos());
        if (newHover != hoveredEdge_) {
            hoveredEdge_ = newHover;
            // Update cursor
            switch (hoveredEdge_) {
            case EdgeLeft:
            case EdgeRight:
                setCursor(Qt::SizeHorCursor);
                break;
            case EdgeTop:
            case EdgeBottom:
                setCursor(Qt::SizeVerCursor);
                break;
            default:
                setCursor(Qt::CrossCursor);
                break;
            }
            update();
        }
    }
}

void RegionSelectorWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        if (draggingEdge_ != EdgeNone) {
            draggingEdge_ = EdgeNone;
            QRect sel = normalizedSelection();
            if (sel.width() > 5 && sel.height() > 5) {
                hasSelection_ = true;
                // Re-normalize after edge drag
                startPos_ = sel.topLeft();
                currentPos_ = sel.bottomRight();
            }
            updateDimensionLabel();
            update();
            return;
        }

        if (selecting_) {
            selecting_ = false;
            currentPos_ = event->pos();

            QRect sel = normalizedSelection();

            // Auto-adjust: snap edges
            if (autoAdjust_ && sel.width() > 5 && sel.height() > 5) {
                QRect snapped = snapSelection(sel);
                startPos_ = snapped.topLeft();
                currentPos_ = snapped.bottomRight();
                sel = snapped;
            }

            if (sel.width() > 5 && sel.height() > 5) {
                hasSelection_ = true;

                // Update selectionMonitor_ to the monitor at the center
                // of the final selection (handles cross-monitor drags)
                QPoint globalCenter = mapToGlobal(sel.center());
                QScreen *centerScreen = QGuiApplication::screenAt(globalCenter);
                if (centerScreen) {
                    qreal dpr = centerScreen->devicePixelRatio();
                    int estPhysX = static_cast<int>(globalCenter.x() * dpr);
                    int estPhysY = static_cast<int>(globalCenter.y() * dpr);
                    POINT pt = { estPhysX, estPhysY };
                    selectionMonitor_ = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
                }

                updateDimensionLabel();
            } else {
                hasSelection_ = false;
                dimensionLabel_->hide();
            }
            update();
        }
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
    // Use the tracked physical monitor for accurate per-monitor DPI conversion.
    // With Per-Monitor V2, Qt's logical coordinates can overlap across screens
    // with different DPRs, so QGuiApplication::screenAt() may return the wrong
    // screen. Using the HMONITOR from GetCursorPos() avoids this ambiguity.

    if (selectionMonitor_) {
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfo(selectionMonitor_, &mi);

        UINT dpiX = 96, dpiY = 96;
        GetDpiForMonitor(selectionMonitor_, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
        qreal dpr = static_cast<qreal>(dpiX) / 96.0;

        // Physical screen origin
        int physOX = mi.rcMonitor.left;
        int physOY = mi.rcMonitor.top;

        // Logical screen origin (as Qt computes: physical / dpr)
        qreal logOX = physOX / dpr;
        qreal logOY = physOY / dpr;

        QPoint globalTL = mapToGlobal(logicalRect.topLeft());

        int physX = physOX + static_cast<int>((globalTL.x() - logOX) * dpr);
        int physY = physOY + static_cast<int>((globalTL.y() - logOY) * dpr);
        int physW = static_cast<int>(logicalRect.width() * dpr);
        int physH = static_cast<int>(logicalRect.height() * dpr);

        return QRect(physX, physY, physW, physH);
    }

    // Fallback: use primary screen DPR
    qreal dpr = devicePixelRatio_;
    QPoint globalCenter = mapToGlobal(logicalRect.center());
    QScreen *targetScreen = QGuiApplication::screenAt(globalCenter);
    if (targetScreen) {
        dpr = targetScreen->devicePixelRatio();
    }

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

    QRect phys = toPhysicalPixels(sel);
    QString text = QString("%1 x %2").arg(phys.width()).arg(phys.height());
    if (hasSelection_) {
        text += QString::fromUtf8("\nEnterキーで確定 / Escでキャンセル");
    }
    dimensionLabel_->setText(text);
    dimensionLabel_->adjustSize();

    int labelX = sel.right() - dimensionLabel_->width();
    int labelY = sel.bottom() + 8;

    if (labelX < 0) labelX = 0;
    if (labelY + dimensionLabel_->height() > height()) {
        labelY = sel.top() - dimensionLabel_->height() - 8;
    }
    if (labelY < 0) labelY = 0;

    dimensionLabel_->move(labelX, labelY);
    dimensionLabel_->show();
}

// ============================================================================
// Auto-Adjust: Screen capture & edge detection
// ============================================================================

void RegionSelectorWidget::captureScreenImage()
{
    // Capture each screen individually and compose into a single image
    // to handle different DPIs correctly
    QRect vd = virtualDesktopGeometry();
    QImage combined(vd.size(), QImage::Format_Grayscale8);
    combined.fill(0);

    for (QScreen *screen : QGuiApplication::screens()) {
        QPixmap pixmap = screen->grabWindow(0);
        QImage img = pixmap.toImage().convertToFormat(QImage::Format_Grayscale8);

        // Scale to logical size (widget coordinates)
        QRect logGeom = screen->geometry();
        QImage scaled = img.scaled(logGeom.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

        // Paint into combined image at the screen's logical position
        QPainter painter(&combined);
        painter.drawImage(logGeom.topLeft() - vd.topLeft(), scaled);
    }

    screenCapture_ = combined;
}

void RegionSelectorWidget::detectEdges()
{
    horizontalEdges_.clear();
    verticalEdges_.clear();

    if (screenCapture_.isNull()) return;

    const int w = screenCapture_.width();
    const int h = screenCapture_.height();
    const int minLineLen = 40; // minimum line length in pixels
    const int diffThreshold = 30; // brightness difference to detect edge

    // Detect horizontal edges (large brightness changes between adjacent rows)
    for (int y = 1; y < h - 1; ++y) {
        const uchar *rowAbove = screenCapture_.scanLine(y - 1);
        const uchar *rowBelow = screenCapture_.scanLine(y);
        int edgeCount = 0;
        for (int x = 0; x < w; x += 4) { // sample every 4 pixels for speed
            int diff = std::abs(static_cast<int>(rowBelow[x]) - static_cast<int>(rowAbove[x]));
            if (diff > diffThreshold) ++edgeCount;
        }
        // If enough of the row has edges, it's a horizontal line
        if (edgeCount > minLineLen / 4) {
            // Avoid duplicates within 3 pixels
            if (horizontalEdges_.empty() || y - horizontalEdges_.back() > 3) {
                horizontalEdges_.push_back(y);
            }
        }
    }

    // Detect vertical edges (large brightness changes between adjacent columns)
    for (int x = 1; x < w - 1; ++x) {
        int edgeCount = 0;
        for (int y = 0; y < h; y += 4) {
            const uchar *row = screenCapture_.scanLine(y);
            int diff = std::abs(static_cast<int>(row[x]) - static_cast<int>(row[x - 1]));
            if (diff > diffThreshold) ++edgeCount;
        }
        if (edgeCount > minLineLen / 4) {
            if (verticalEdges_.empty() || x - verticalEdges_.back() > 3) {
                verticalEdges_.push_back(x);
            }
        }
    }
}

int RegionSelectorWidget::snapToEdge(int pos, bool horizontal) const
{
    const auto &edges = horizontal ? horizontalEdges_ : verticalEdges_;
    int bestDist = SNAP_DISTANCE + 1;
    int bestEdge = pos;

    for (int edge : edges) {
        int dist = std::abs(edge - pos);
        if (dist < bestDist) {
            bestDist = dist;
            bestEdge = edge;
        }
        if (edge > pos + SNAP_DISTANCE) break; // edges are sorted
    }
    return bestEdge;
}

QRect RegionSelectorWidget::snapSelection(const QRect &sel) const
{
    int left   = snapToEdge(sel.left(),   false);
    int right  = snapToEdge(sel.right(),  false);
    int top    = snapToEdge(sel.top(),    true);
    int bottom = snapToEdge(sel.bottom(), true);
    return QRect(QPoint(left, top), QPoint(right, bottom));
}

RegionSelectorWidget::Edge RegionSelectorWidget::hitTestEdge(const QPoint &pos) const
{
    if (!hasSelection_) return EdgeNone;

    QRect sel = normalizedSelection();
    const int margin = 8;

    // Check if pos is near any edge
    bool nearLeft   = std::abs(pos.x() - sel.left())   <= margin && pos.y() >= sel.top() - margin && pos.y() <= sel.bottom() + margin;
    bool nearRight  = std::abs(pos.x() - sel.right())  <= margin && pos.y() >= sel.top() - margin && pos.y() <= sel.bottom() + margin;
    bool nearTop    = std::abs(pos.y() - sel.top())    <= margin && pos.x() >= sel.left() - margin && pos.x() <= sel.right() + margin;
    bool nearBottom = std::abs(pos.y() - sel.bottom()) <= margin && pos.x() >= sel.left() - margin && pos.x() <= sel.right() + margin;

    if (nearLeft)   return EdgeLeft;
    if (nearRight)  return EdgeRight;
    if (nearTop)    return EdgeTop;
    if (nearBottom) return EdgeBottom;
    return EdgeNone;
}
