#pragma once

#include <QWidget>

#include <array>
#include <QStringList>

class QLabel;
class QLineEdit;
class QSlider;
class QTableWidget;
class QTimer;

namespace NodeGUI::runtime {

class RuntimeController;

// Signal-assignment panel: plot-view slider, filter, and the live signal
// table with G1/G2/G3 checkboxes and current values. Dockable counterpart of
// the plot widgets — emits when assignments or the view window change.
class SignalTablePanel : public QWidget {
    Q_OBJECT

public:
    // Slider travel for the logarithmic plot-view control.
    static constexpr int kViewSliderSteps = 1000;

    explicit SignalTablePanel(RuntimeController* controller, QWidget* parent = nullptr);

    // The signal sets assigned to each of the three graphs (layout presets).
    std::array<QStringList, 3> GraphSignalSets() const;
    void SetGraphSignalSets(const std::array<QStringList, 3>& sets);

    // Moves the slider and notifies the plots (used by the preset buttons).
    void SetViewSeconds(double seconds);

signals:
    void graphSignalsChanged(const std::array<QStringList, 3>& sets);
    void viewSecondsChanged(double seconds);

private slots:
    void OnStoreChanged();
    void OnFilterChanged(const QString& text);
    void OnViewSecondsChanged(int value);
    void RefreshValues();

private:
    void RebuildSignalTable();
    void SyncCheckStates();

    RuntimeController* controller_;

    QSlider* viewSlider_ = nullptr;
    QLabel* viewValueLabel_ = nullptr;
    QLineEdit* filterEdit_ = nullptr;
    QTableWidget* signalTable_ = nullptr;
    QTimer* valueTimer_ = nullptr;

    std::array<QStringList, 3> graphSignals_;
    bool rebuildingTable_ = false;
    bool valuesDirty_ = false;
    std::size_t knownSignalCount_ = 0;
};

}  // namespace NodeGUI::runtime
