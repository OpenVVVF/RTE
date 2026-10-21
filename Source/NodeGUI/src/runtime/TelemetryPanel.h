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
//
// Also the surface hub for the playback cursor: leaf cursor gestures
// (cursorDragged / cursorClearRequested / panRequested) land here, get
// fanned out to every plot, and are re-emitted as cursorChanged so the
// signal table's Value column can follow (RuntimeTab wires that). A pinned
// cursor holds the window span while the live stream keeps running behind
// it; clearing re-tails. The small cursorInfo_ label doubles as the
// headless-smoke read dump.
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

    // Shared-cursor operations — the leaf gesture signals connect straight
    // here, and the headless smoke driver pokes the same entry points.
    void DragCursorTo(double tsec);
    void ClearCursor();
    void PanWindowBy(double deltaTsec);

signals:
    void graphCountChanged(int count);
    void cursorChanged(bool hasCursor, double tsec);

private slots:
    void OnStoreChanged();

private:
    SignalPlotWidget* MakePlot();
    void UpdateCursorInfo();

    RuntimeController* controller_;

    QScrollArea* scroll_ = nullptr;
    QWidget* plotsContainer_ = nullptr;
    QVBoxLayout* plotsLayout_ = nullptr;
    QLabel* countLabel_ = nullptr;
    QLabel* cursorInfo_ = nullptr;

    std::vector<SignalPlotWidget*> plots_;
    bool hasCursor_ = false;
    double cursorTsec_ = 0.0;
};

}  // namespace NodeGUI::runtime
