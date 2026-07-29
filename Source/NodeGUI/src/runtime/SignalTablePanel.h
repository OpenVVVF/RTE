#pragma once

#include <QWidget>

#include <array>
#include <QStringList>

class QLineEdit;
class QSlider;
class QTableWidget;

namespace NodeGUI::runtime {

class RuntimeController;

// Signal-assignment panel: plot-view slider, filter, and the live signal
// table with G1/G2/G3 checkboxes and current values. Dockable counterpart of
// the plot widgets — emits when assignments or the view window change.
class SignalTablePanel : public QWidget {
    Q_OBJECT

public:
    explicit SignalTablePanel(RuntimeController* controller, QWidget* parent = nullptr);

    // The signal sets assigned to each of the three graphs (layout presets).
    std::array<QStringList, 3> GraphSignalSets() const;
    void SetGraphSignalSets(const std::array<QStringList, 3>& sets);

signals:
    void graphSignalsChanged(const std::array<QStringList, 3>& sets);
    void viewSecondsChanged(double seconds);

private slots:
    void OnStoreChanged();
    void OnFilterChanged(const QString& text);
    void OnViewSecondsChanged(int value);

private:
    void RebuildSignalTable();

    RuntimeController* controller_;

    QSlider* viewSlider_ = nullptr;
    QLineEdit* filterEdit_ = nullptr;
    QTableWidget* signalTable_ = nullptr;

    std::array<QStringList, 3> graphSignals_;
    bool rebuildingTable_ = false;
};

}  // namespace NodeGUI::runtime
