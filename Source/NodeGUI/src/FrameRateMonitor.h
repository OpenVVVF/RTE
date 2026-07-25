#pragma once

#include <QObject>
#include <QPointer>
#include <QTimer>

#include <chrono>
#include <deque>

class QLabel;
class QWidget;
class QEvent;

namespace NodeGUI {

// Displays an FPS / frametime overlay in the top-right corner of a graphics view.
class FrameRateMonitor : public QObject {
    Q_OBJECT

public:
    explicit FrameRateMonitor(QWidget* viewWidget, QObject* parent = nullptr);

private slots:
    void UpdateDisplay();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void RepositionLabel();
    QWidget* Viewport() const;

    QPointer<QWidget> viewWidget_;
    QLabel* label_ = nullptr;
    QTimer* updateTimer_ = nullptr;

    // Timestamps (ms) of recent viewport paint events.
    std::deque<qint64> paintTimestamps_;
};

}  // namespace NodeGUI
