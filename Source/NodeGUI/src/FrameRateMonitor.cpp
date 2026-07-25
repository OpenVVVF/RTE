#include "FrameRateMonitor.h"

#include <QEvent>
#include <QGraphicsView>
#include <QLabel>
#include <QWidget>

#include <algorithm>

namespace NodeGUI {

namespace {

constexpr int kUpdateIntervalMs = 250;
constexpr int kSampleWindowMs = 1000;
constexpr int kMargin = 8;

}  // namespace

FrameRateMonitor::FrameRateMonitor(QWidget* viewWidget, QObject* parent)
    : QObject(parent)
    , viewWidget_(viewWidget)
    , label_(new QLabel(viewWidget))
    , updateTimer_(new QTimer(this)) {
    label_->setStyleSheet(
        QStringLiteral("QLabel {"
                       "  color: #D0D0D0;"
                       "  background-color: rgba(48, 48, 48, 160);"
                       "  border: 1px solid rgba(96, 96, 96, 200);"
                       "  border-radius: 4px;"
                       "  padding: 4px 8px;"
                       "  font-family: monospace;"
                       "  font-size: 12px;"
                       "}"));
    label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    label_->setText(QStringLiteral("FPS: --\nFrame: -- ms"));
    label_->setAttribute(Qt::WA_TransparentForMouseEvents);

    if (viewWidget_) {
        viewWidget_->installEventFilter(this);
    }

    if (QWidget* viewport = Viewport()) {
        viewport->installEventFilter(this);
    }

    connect(updateTimer_, &QTimer::timeout, this, &FrameRateMonitor::UpdateDisplay);
    updateTimer_->start(kUpdateIntervalMs);

    RepositionLabel();
}

QWidget* FrameRateMonitor::Viewport() const {
    if (auto* graphicsView = qobject_cast<QGraphicsView*>(viewWidget_)) {
        return graphicsView->viewport();
    }
    return viewWidget_;
}

bool FrameRateMonitor::eventFilter(QObject* watched, QEvent* event) {
    if (watched == Viewport() && event->type() == QEvent::Paint) {
        paintTimestamps_.push_back(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());

        // Drop timestamps older than the sample window.
        const qint64 now = paintTimestamps_.back();
        while (!paintTimestamps_.empty() && paintTimestamps_.front() < now - kSampleWindowMs) {
            paintTimestamps_.pop_front();
        }
    } else if (watched == viewWidget_ &&
               (event->type() == QEvent::Resize || event->type() == QEvent::Move)) {
        RepositionLabel();
    }

    return false;
}

void FrameRateMonitor::UpdateDisplay() {
    if (!viewWidget_ || !label_) {
        return;
    }

    const qint64 now =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();

    // Drop old samples.
    while (!paintTimestamps_.empty() && paintTimestamps_.front() < now - kSampleWindowMs) {
        paintTimestamps_.pop_front();
    }

    if (paintTimestamps_.size() < 2) {
        label_->setText(QStringLiteral("FPS: --\nFrame: -- ms"));
        RepositionLabel();
        return;
    }

    const qint64 oldest = paintTimestamps_.front();
    const qint64 newest = paintTimestamps_.back();
    const double elapsedSeconds = static_cast<double>(newest - oldest) / 1000.0;
    const int frameCount = static_cast<int>(paintTimestamps_.size()) - 1;
    const double fps = elapsedSeconds > 0.0 ? static_cast<double>(frameCount) / elapsedSeconds : 0.0;
    const double frameTimeMs = frameCount > 0 ? elapsedSeconds * 1000.0 / frameCount : 0.0;

    label_->setText(QStringLiteral("FPS: %1\nFrame: %2 ms")
                        .arg(fps, 0, 'f', 1)
                        .arg(frameTimeMs, 0, 'f', 2));

    RepositionLabel();
}

void FrameRateMonitor::RepositionLabel() {
    if (!viewWidget_ || !label_) {
        return;
    }

    label_->adjustSize();

    QRect anchorRect;
    if (auto* graphicsView = qobject_cast<QGraphicsView*>(viewWidget_)) {
        // Anchor to the visible viewport area, which may be inset inside the view.
        if (QWidget* viewport = graphicsView->viewport()) {
            anchorRect = viewport->geometry();
        }
    }

    if (!anchorRect.isValid()) {
        anchorRect = viewWidget_->rect();
    }

    const QSize labelSize = label_->size();
    const int x = std::max(kMargin,
                           anchorRect.right() - labelSize.width() - kMargin + 1);
    const int y = anchorRect.top() + kMargin;

    label_->move(x, y);
    label_->raise();
}

}  // namespace NodeGUI
