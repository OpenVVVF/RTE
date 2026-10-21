#pragma once

#include <QWidget>

#include <vector>

class QLabel;
class QScrollArea;
class QVBoxLayout;

namespace NodeGUI::runtime {

class RuntimeController;
class SignalPlotWidget;

// Plot region of the Runtime telemetry view: N SignalPlotWidget leaf
// widgets in a scrolling column with an add/remove toolbar. The default
// count of three matches the historical layout; assignments arrive from
// SignalTablePanel (single-target dropdown per signal).
class TelemetryPanel : public QWidget {
    Q_OBJECT

public:
    explicit TelemetryPanel(RuntimeController* controller, QWidget* parent = nullptr);

    int GraphCount() const { return static_cast<int>(plots_.size()); }
    void SetGraphSignals(const std::vector<QStringList>& sets);

public slots:
    void SetViewSeconds(double seconds);
    void AddGraph();
    void RemoveLastGraph();
    // Snap the visible graph count to a target (preset/autosave load path):
    // assignment drops for removed graphs are the caller's business.
    void SetGraphCount(int count);

signals:
    void graphCountChanged(int count);

private slots:
    void OnStoreChanged();

private:
    SignalPlotWidget* MakePlot();

    RuntimeController* controller_;

    QScrollArea* scroll_ = nullptr;
    QWidget* plotsContainer_ = nullptr;
    QVBoxLayout* plotsLayout_ = nullptr;
    QLabel* countLabel_ = nullptr;

    std::vector<SignalPlotWidget*> plots_;
};

}  // namespace NodeGUI::runtime
