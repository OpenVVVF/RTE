#pragma once

#include "SimSession.h"

#include "runtime/TelemetryStore.h"

#include <QWidget>

#include <memory>
#include <vector>
#include <QStringList>

class QComboBox;
class QLabel;
class QLineEdit;
class QScrollArea;
class QSlider;
class QStackedWidget;
class QTableWidget;
class QTimer;
class QVBoxLayout;

namespace NodeGUI::runtime {
class SignalPlotWidget;
}

namespace NodeGUI::sim {

// Telemetry region of the Simulate screen: signal list with a per-signal
// target-graph dropdown on the left, N runtime plot leaf widgets in a
// scrolling column center-right (add/remove toolbar; default 3 like the
// historical layout), and the session summary line underneath.
//
// Data path: SimSession (v3 jsonl on disk) -> runtime::TelemetryStore ->
// runtime::SignalPlotWidget — the same contract the live Runtime tab uses,
// so sim and recorded sessions render identically. Mirrors the runtime
// TelemetryPanel + SignalTablePanel machinery.
//
// Layout persistence: graph count + per-graph signal sets autosave per
// session class (file stem + channel-set hash) into QSettings, mirroring
// RuntimeTab's autosave shape; a loaded session with a stored entry restores
// it instead of falling back to the first-load favorites.
class SimTelemetryView : public QWidget {
    Q_OBJECT

public:
    explicit SimTelemetryView(QWidget* parent = nullptr);
    ~SimTelemetryView() override;

    // Loads and renders a session file. On failure returns false and keeps
    // whatever was showing before; never throws — all-null channels list as
    // grayed rows, sparse channels just drop their null samples off-plots.
    bool LoadSession(const QString& path, QString& error);
    const SimSession* Session() const { return session_.get(); }
    NodeGUI::runtime::TelemetryStore& Store() { return store_; }

private slots:
    void AddGraph();
    void RemoveLastGraph();

private:
    void RebuildSignalTable();
    void ApplyAssignments();
    void ApplyDefaultAssignments();
    void UpdateStatusLine();
    void ApplyViewSeconds();
    NodeGUI::runtime::SignalPlotWidget* MakePlot();
    void UpdateGraphCount();
    int AssignedGraph(const QString& name) const;
    // Per-session-class layout persistence (see the class comment): restore
    // runs inside LoadSession ahead of the default-assignment fallback;
    // ScheduleLayoutSave debounces every user-side change (combo target
    // edit, graph add/remove) and SaveLayout writes it through.
    bool TryRestoreLayout();
    void ScheduleLayoutSave();
    void SaveLayout();

    NodeGUI::runtime::TelemetryStore store_;
    std::unique_ptr<SimSession> session_;
    QString sessionPath_;  // full path of the loaded session (status line)

    QStackedWidget* stack_ = nullptr;  // empty-state page vs content page
    QLineEdit* filterEdit_ = nullptr;
    QTableWidget* signalTable_ = nullptr;
    QSlider* viewSlider_ = nullptr;
    QScrollArea* scroll_ = nullptr;
    QWidget* plotsContainer_ = nullptr;
    QVBoxLayout* plotsLayout_ = nullptr;
    QLabel* countLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;

    std::vector<NodeGUI::runtime::SignalPlotWidget*> plots_;
    std::vector<QStringList> graphSignals_{QStringList{}, QStringList{}, QStringList{}};
    bool rebuildingTable_ = false;

    QTimer* layoutSaveTimer_ = nullptr;
    QString layoutStem_;         // sanitized filename stem of the loaded session
    QString layoutChannelHash_;  // hash of its float channel names
};

}  // namespace NodeGUI::sim
