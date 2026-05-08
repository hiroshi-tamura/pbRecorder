#include "UiElementSelectorWidget.h"

#include <QCursor>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QShowEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

int rectDistance(const pb::RegionRect& a, const pb::RegionRect& b)
{
    return std::abs(a.x - b.x) +
           std::abs(a.y - b.y) +
           std::abs(a.width - b.width) +
           std::abs(a.height - b.height);
}

} // namespace

UiElementSelectorWidget::UiElementSelectorWidget(QWidget *parent)
    : QWidget(parent)
{
    setWindowFlags(Qt::FramelessWindowHint
                   | Qt::WindowStaysOnTopHint
                   | Qt::Tool);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose);
    setCursor(Qt::PointingHandCursor);
    setMouseTracking(true);

    infoLabel_ = new QLabel(this);
    infoLabel_->setStyleSheet(
        "QLabel {"
        "  background-color: rgba(0, 0, 0, 190);"
        "  color: white;"
        "  padding: 6px 10px;"
        "  border-radius: 3px;"
        "  font-size: 12px;"
        "}");
    infoLabel_->hide();

    connect(&refreshTimer_, &QTimer::timeout, this, &UiElementSelectorWidget::refreshCandidate);

    setGeometry(virtualDesktopGeometry());
}

UiElementSelectorWidget::~UiElementSelectorWidget() = default;

void UiElementSelectorWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);

    HWND hwnd = reinterpret_cast<HWND>(winId());
    SetWindowPos(hwnd, HWND_TOPMOST,
                 GetSystemMetrics(SM_XVIRTUALSCREEN),
                 GetSystemMetrics(SM_YVIRTUALSCREEN),
                 GetSystemMetrics(SM_CXVIRTUALSCREEN),
                 GetSystemMetrics(SM_CYVIRTUALSCREEN),
                 SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd);

    refreshCandidate();
    refreshTimer_.start(80);
    activateWindow();
    setFocus();
}

void UiElementSelectorWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.fillRect(rect(), QColor(0, 0, 0, 90));

    if (!hasCandidate_) {
        return;
    }

    QRect highlight = toLogicalRect(currentPreview_.rect);
    highlight = highlight.intersected(rect());
    if (highlight.isEmpty()) {
        return;
    }

    painter.setCompositionMode(QPainter::CompositionMode_Clear);
    painter.fillRect(highlight, Qt::transparent);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

    QPen pen(QColor(80, 180, 255), 3);
    painter.setPen(pen);
    painter.setBrush(QColor(80, 180, 255, 35));
    painter.drawRect(highlight.adjusted(0, 0, -1, -1));
}

void UiElementSelectorWidget::mouseMoveEvent(QMouseEvent *event)
{
    Q_UNUSED(event);
    refreshCandidate();
}

void UiElementSelectorWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && hasCandidate_) {
        confirmCurrent();
        return;
    }
    QWidget::mousePressEvent(event);
}

void UiElementSelectorWidget::wheelEvent(QWheelEvent *event)
{
    if (candidates_.empty()) {
        return;
    }

    const int delta = event->angleDelta().y();
    if (delta < 0) {
        candidateIndex_ = std::min(candidateIndex_ + 1, static_cast<int>(candidates_.size()) - 1);
    } else if (delta > 0) {
        candidateIndex_ = std::max(candidateIndex_ - 1, 0);
    }

    currentPreview_ = candidates_[static_cast<size_t>(candidateIndex_)];
    hasCandidate_ = true;
    updateInfoLabel();
    update();
}

void UiElementSelectorWidget::keyPressEvent(QKeyEvent *event)
{
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && hasCandidate_) {
        confirmCurrent();
        return;
    }

    if (event->key() == Qt::Key_Escape) {
        emit selectionCancelled();
        close();
        return;
    }

    if (event->key() == Qt::Key_Space && (event->modifiers() & Qt::ControlModifier) && hasCandidate_) {
        confirmCurrent();
        return;
    }

    if ((event->key() == Qt::Key_Up || event->key() == Qt::Key_Left) && !candidates_.empty()) {
        candidateIndex_ = std::max(candidateIndex_ - 1, 0);
        currentPreview_ = candidates_[static_cast<size_t>(candidateIndex_)];
        hasCandidate_ = true;
        updateInfoLabel();
        update();
        return;
    }

    if ((event->key() == Qt::Key_Down || event->key() == Qt::Key_Right) && !candidates_.empty()) {
        candidateIndex_ = std::min(candidateIndex_ + 1, static_cast<int>(candidates_.size()) - 1);
        currentPreview_ = candidates_[static_cast<size_t>(candidateIndex_)];
        hasCandidate_ = true;
        updateInfoLabel();
        update();
        return;
    }

    QWidget::keyPressEvent(event);
}

QRect UiElementSelectorWidget::virtualDesktopGeometry() const
{
    int physX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int physY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int physW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int physH = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    QScreen *primary = QGuiApplication::primaryScreen();
    qreal dpr = primary ? primary->devicePixelRatio() : 1.0;

    return QRect(
        static_cast<int>(std::floor(physX / dpr)),
        static_cast<int>(std::floor(physY / dpr)),
        static_cast<int>(std::ceil(physW / dpr)),
        static_cast<int>(std::ceil(physH / dpr)));
}

QRect UiElementSelectorWidget::toLogicalRect(const pb::RegionRect& rect) const
{
    HWND hwnd = reinterpret_cast<HWND>(const_cast<UiElementSelectorWidget*>(this)->winId());
    RECT winRect{};
    GetWindowRect(hwnd, &winRect);

    int logW = width();
    int logH = height();
    int physW = winRect.right - winRect.left;
    int physH = winRect.bottom - winRect.top;

    qreal dprX = (logW > 0) ? static_cast<qreal>(physW) / logW : 1.0;
    qreal dprY = (logH > 0) ? static_cast<qreal>(physH) / logH : 1.0;

    return QRect(
        static_cast<int>((rect.x - winRect.left) / dprX),
        static_cast<int>((rect.y - winRect.top) / dprY),
        static_cast<int>(rect.width / dprX),
        static_cast<int>(rect.height / dprY));
}

void UiElementSelectorWidget::refreshCandidate()
{
    POINT pt{};
    GetCursorPos(&pt);
    HWND self = reinterpret_cast<HWND>(winId());

    LONG_PTR exStyle = GetWindowLongPtrW(self, GWL_EXSTYLE);
    SetWindowLongPtrW(self, GWL_EXSTYLE, exStyle | WS_EX_TRANSPARENT);
    auto nextCandidates = uiAutomation_.previewCandidatesFromPoint(pt);
    SetWindowLongPtrW(self, GWL_EXSTYLE, exStyle);
    nextCandidates.erase(
        std::remove_if(nextCandidates.begin(), nextCandidates.end(),
            [self](const pb::UiElementPreview& candidate) {
                HWND root = candidate.rootWindow;
                return root == self || IsChild(self, root) ||
                       candidate.rect.width <= 1 ||
                       candidate.rect.height <= 1;
            }),
        nextCandidates.end());

    if (nextCandidates.empty()) {
        candidates_.clear();
        candidateIndex_ = 0;
        hasCandidate_ = false;
        infoLabel_->setText(currentLang_ == "ja"
            ? QString::fromUtf8("UI領域が見つかりません")
            : QStringLiteral("No UI region found"));
        infoLabel_->adjustSize();
        infoLabel_->move(16, 16);
        infoLabel_->show();
        update();
        return;
    }

    bool sameCurrent = false;
    if (hasCandidate_) {
        for (int i = 0; i < static_cast<int>(nextCandidates.size()); ++i) {
            const auto& t = nextCandidates[static_cast<size_t>(i)];
            if (t.rootWindow == currentPreview_.rootWindow &&
                t.rect.x == currentPreview_.rect.x &&
                t.rect.y == currentPreview_.rect.y &&
                t.rect.width == currentPreview_.rect.width &&
                t.rect.height == currentPreview_.rect.height) {
                candidateIndex_ = i;
                sameCurrent = true;
                break;
            }
        }
    }

    candidates_ = std::move(nextCandidates);
    if (!sameCurrent) {
        candidateIndex_ = std::min(candidateIndex_, static_cast<int>(candidates_.size()) - 1);
    }
    currentPoint_ = pt;
    currentPreview_ = candidates_[static_cast<size_t>(candidateIndex_)];
    hasCandidate_ = true;
    updateInfoLabel();
    update();
}

void UiElementSelectorWidget::updateInfoLabel()
{
    if (!hasCandidate_) {
        infoLabel_->hide();
        return;
    }

    QString name = QString::fromStdWString(currentPreview_.name);
    if (name.trimmed().isEmpty()) {
        name = QString::fromStdWString(currentPreview_.automationId);
    }
    if (name.trimmed().isEmpty()) {
        name = QString::fromStdWString(currentPreview_.className);
    }
    if (name.trimmed().isEmpty()) {
        name = currentLang_ == "ja" ? QStringLiteral("UI領域") : QStringLiteral("UI region");
    }
    const auto& r = currentPreview_.rect;
    QString text = currentLang_ == "ja"
        ? QString::fromUtf8("%1\n%2 x %3\nCtrl+Space/Enter/クリックで確定、Escでキャンセル")
        : QStringLiteral("%1\n%2 x %3\nCtrl+Space/Enter/click to select, Esc to cancel");
    infoLabel_->setText(text.arg(name).arg(r.width).arg(r.height));
    infoLabel_->adjustSize();

    QRect highlight = toLogicalRect(r);
    int x = highlight.left();
    int y = highlight.bottom() + 8;
    if (x + infoLabel_->width() > width()) {
        x = width() - infoLabel_->width() - 8;
    }
    if (y + infoLabel_->height() > height()) {
        y = highlight.top() - infoLabel_->height() - 8;
    }
    x = std::max(8, x);
    y = std::max(8, y);
    infoLabel_->move(x, y);
    infoLabel_->show();
}

bool UiElementSelectorWidget::confirmCurrent()
{
    if (!hasCandidate_) {
        return false;
    }

    GetCursorPos(&currentPoint_);
    HWND self = reinterpret_cast<HWND>(winId());
    ShowWindow(self, SW_HIDE);
    Sleep(30);

    auto candidates = uiAutomation_.candidatesFromPoint(currentPoint_);
    if (candidates.empty()) {
        ShowWindow(self, SW_SHOW);
        SetForegroundWindow(self);
        return false;
    }

    auto best = candidates.end();
    int bestScore = std::numeric_limits<int>::max();
    for (auto it = candidates.begin(); it != candidates.end(); ++it) {
        if (it->target.rootWindow == self || IsChild(self, it->target.rootWindow)) {
            continue;
        }
        int score = rectDistance(it->target.initialRect, currentPreview_.rect);
        if (score < bestScore) {
            bestScore = score;
            best = it;
        }
    }

    if (best == candidates.end()) {
        ShowWindow(self, SW_SHOW);
        SetForegroundWindow(self);
        return false;
    }

    pb::UiElementTarget target = best->target;
    target.initialRect = currentPreview_.rect;
    emit uiElementSelected(target);
    close();
    return true;
}
