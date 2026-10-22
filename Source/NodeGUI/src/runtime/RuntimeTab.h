#pragma once

#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;

namespace NodeGUI::runtime {

class ConsolePanel;
class FramKeysManager;
class RuntimeController;
class SignalTablePanel;
class TelemetryPanel;

// Top-level widget of the "Runtime" tab: link-status header, graph-layout
// preset row, and telemetry plots. The signal table and console live in dock
// widgets on the main window (see GetSignalTable / GetConsole); this widget
// wires them to the plots.
//
// Layout autosave: besides closeEvent's SaveAutosave, a 500 ms debounce
// timer writes user-side layout edits (combo target, graph add/remove,
// pane reorder, pane heights) as they happen — same write-through discipline
// as the sim view's ScheduleLayoutSave.
class RuntimeTab : public QWidget {
    Q_OBJECT

public:
    RuntimeTab(RuntimeController* controller,
               QWidget* parent = nullptr);

    // Dockable panels owned by this tab; the main window places them.
    SignalTablePanel* GetSignalTable() const { return signalTablePanel_; }
    ConsolePanel* GetConsole() const { return consolePanel_; }

    // Persisted graph-layout presets (signal sets of the three plots).
    void LoadAutosave();
    void SaveAutosave();

public slots:
    void OnSaveFramKeys();
    void OnLoadFramKeys();

private slots:
    void OnStoreChanged();
    void OnSavePreset();
    void OnLoadPreset();
    void OnExportSession();
    void OnClearSession();

private:
    void RefreshRecentCombo();
    void ScheduleAutosave();

    RuntimeController* controller_;
    FramKeysManager* framKeysManager_ = nullptr;

    QLabel* headerLabel_ = nullptr;
    // Amber "no telemetry" banner - waits for sustained 0 Hz (N consecutive
    // OnStoreChanged reads once the runtime is established) rather than
    // flashing at startup (workflow dossier J3.2 / coldboot: the runtime
    // header could show 0 Hz forever silently).
    QLabel* noTelemetryBanner_ = nullptr;
    int consecutiveZeroHzReads_ = 0;
    QLabel* exportStatus_ = nullptr;
    QLineEdit* presetNameEdit_ = nullptr;
    QComboBox* recentCombo_ = nullptr;
    QLabel* presetStatus_ = nullptr;
    TelemetryPanel* telemetryPanel_ = nullptr;
    SignalTablePanel* signalTablePanel_ = nullptr;
    ConsolePanel* consolePanel_ = nullptr;
    QTimer* autosaveTimer_ = nullptr;
};

}  // namespace NodeGUI::runtime
