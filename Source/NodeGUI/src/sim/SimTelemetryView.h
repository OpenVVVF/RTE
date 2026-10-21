#pragma once

#include "SimSession.h"

#include "runtime/TelemetryStore.h"

#include <QWidget>

#include <array>
#include <memory>

class QCheckBox;
class QLabel;
class QLineEdit;
class QStackedWidget;
class QTableWidget;

namespace NodeGUI::runtime {
class SignalPlotWidget;
}

namespace NodeGUI::sim {

// Telemetry region of the Simulate screen: signal list with G1/G2/G3
// check-box assignment on the left, the three runtime plot leaf widgets
// center-right, and the session summary line underneath.
//
// Data path: SimSession (v3 jsonl on disk) -> runtime::TelemetryStore ->
// runtime::SignalPlotWidget — the same contract the live Runtime tab uses,
// so sim and recorded sessions render identically. Composition mirrors
// runtime/TelemetryPanel + SignalTablePanel.
class SimTelemetryView : public QWidget {
    Q_OBJECT

public:
    explicit SimTelemetryView(QWidget* parent = nullptr);

    // Loads and renders a session file. On failure returns false and keeps
    // whatever was showing before; never throws — all-null channels list as
    // grayed rows, sparse channels just drop their null samples off-plots.
    bool LoadSession(const QString& path, QString& error);
    const SimSession* Session() const { return session_.get(); }
    NodeGUI::runtime::TelemetryStore& Store() { return store_; }

private:
    void RebuildSignalTable();
    void ApplyAssignments();
    void ApplyDefaultAssignments();
    void UpdateStatusLine();

    NodeGUI::runtime::TelemetryStore store_;
    std::unique_ptr<SimSession> session_;

    QStackedWidget* stack_ = nullptr;  // empty-state page vs content page
    QLineEdit* filterEdit_ = nullptr;
    QTableWidget* signalTable_ = nullptr;
    std::array<NodeGUI::runtime::SignalPlotWidget*, 3> plots_{};
    QLabel* statusLabel_ = nullptr;

    std::array<QStringList, 3> graphSignals_;
    bool rebuildingTable_ = false;
};

}  // namespace NodeGUI::sim
