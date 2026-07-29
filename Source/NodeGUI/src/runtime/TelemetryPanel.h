#pragma once

#include <QWidget>

#include <array>
#include <QStringList>
#include <vector>

class QCheckBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSlider;
class QTableWidget;

namespace NodeGUI::runtime {

class RuntimeController;
class SignalPlotWidget;

// Telemetry view: left side has the plot-view slider, signal filter, the
// signal-assignment table (G1/G2/G3 checkboxes) and the device console; right
// side has three stacked GPU plot widgets. Mirrors the old ImGui "Telemetry"
// tab layout.
class TelemetryPanel : public QWidget {
    Q_OBJECT

public:
    explicit TelemetryPanel(RuntimeController* controller, QWidget* parent = nullptr);

    // The signal sets assigned to each of the three graphs (layout presets).
    std::array<QStringList, 3> GraphSignalSets() const;
    void SetGraphSignalSets(const std::array<QStringList, 3>& sets);

private slots:
    void OnStoreChanged();
    void OnSendCommand();
    void OnFilterChanged(const QString& text);
    void OnViewSecondsChanged(int value);

private:
    void RebuildSignalTable();
    void ApplyGraphAssignments();
    void AppendConsoleLines();

    RuntimeController* controller_;

    QSlider* viewSlider_ = nullptr;
    QLineEdit* filterEdit_ = nullptr;
    QTableWidget* signalTable_ = nullptr;
    QPlainTextEdit* consoleView_ = nullptr;
    QLineEdit* commandEdit_ = nullptr;
    QCheckBox* autoscrollCheck_ = nullptr;

    std::array<SignalPlotWidget*, 3> plots_{};
    std::array<QStringList, 3> graphSignals_;

    // Console drain position (store console seq).
    uint64_t lastConsoleSeq_ = 0;
    bool rebuildingTable_ = false;

    QStringList commandHistory_;
    int historyIndex_ = -1;
};

}  // namespace NodeGUI::runtime
