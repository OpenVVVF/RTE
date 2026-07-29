#pragma once

#include <QWidget>

#include <array>
#include <QStringList>

namespace NodeGUI::runtime {

class RuntimeController;
class SignalPlotWidget;

// Telemetry plot view: the three stacked GPU plot widgets. Signal assignment
// and the view window are driven externally (by SignalTablePanel).
class TelemetryPanel : public QWidget {
    Q_OBJECT

public:
    explicit TelemetryPanel(RuntimeController* controller, QWidget* parent = nullptr);

public slots:
    void SetGraphSignals(const std::array<QStringList, 3>& sets);
    void SetViewSeconds(double seconds);

private slots:
    void OnStoreChanged();

private:
    RuntimeController* controller_;
    std::array<SignalPlotWidget*, 3> plots_{};
};

}  // namespace NodeGUI::runtime
