#pragma once

#include <QWidget>

#include <array>
#include <QStringList>

class QTimer;

namespace NodeGUI::runtime {

class RuntimeController;
class SignalPlotWidget;

// Telemetry plot view: the three stacked GPU plot widgets. Signal assignment
// and the view window are driven externally (by SignalTablePanel).
class TelemetryPanel : public QWidget {
    Q_OBJECT

public:
    explicit TelemetryPanel(RuntimeController* controller, QWidget* parent = nullptr);

signals:
    // Forwarded from a plot when the user drags out a time span.
    void viewSecondsRequested(double seconds);

public slots:
    void SetGraphSignals(const std::array<QStringList, 3>& sets);
    void SetViewSeconds(double seconds);
    void SetGraphViewSeconds(const std::array<double, 3>& seconds);

private slots:
    void OnPlotTimeFreeze(bool frozen, double anchorSimSec);
    void RefreshPlots();

private:
    RuntimeController* controller_;
    std::array<SignalPlotWidget*, 3> plots_{};
    QTimer* plotRefreshTimer_ = nullptr;
};

}  // namespace NodeGUI::runtime
