#pragma once

#include <QWidget>

#include <vector>
#include <QStringList>

class QComboBox;
class QLineEdit;
class QSlider;
class QTableWidget;

namespace NodeGUI::runtime {

class RuntimeController;

// Signal-assignment panel: plot-view slider, filter, and the live signal
// table with a per-signal target-graph dropdown ("—" = unassigned, G1..Gn
// = that graph) plus current values. Generalized from the fixed G1/G2/G3
// checkbox columns: TelemetryPanel owns the graph count and emits
// graphCountChanged -> SetGraphCount here.
class SignalTablePanel : public QWidget {
    Q_OBJECT

public:
    explicit SignalTablePanel(RuntimeController* controller, QWidget* parent = nullptr);

    // Assigned signal sets per graph (size == graph count).
    std::vector<QStringList> GraphSignalSets() const;
    void SetGraphSignalSets(const std::vector<QStringList>& sets);

public slots:
    void SetGraphCount(int count);

signals:
    void graphSignalsChanged(const std::vector<QStringList>& sets);
    void viewSecondsChanged(double seconds);

private slots:
    void OnStoreChanged();
    void OnFilterChanged(const QString& text);
    void OnViewSecondsChanged(int value);

private:
    void RebuildSignalTable();
    void PopulateTargetCombo(QComboBox* combo) const;
    int AssignedGraph(const QString& name) const;

    RuntimeController* controller_;

    QSlider* viewSlider_ = nullptr;
    QLineEdit* filterEdit_ = nullptr;
    QTableWidget* signalTable_ = nullptr;

    std::vector<QStringList> graphSignals_{QStringList{}, QStringList{}, QStringList{}};
    bool rebuildingTable_ = false;
};

}  // namespace NodeGUI::runtime
