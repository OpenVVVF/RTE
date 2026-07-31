#pragma once

#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace NodeGUI::runtime {

class ConsolePanel;
class RuntimeController;
class SignalTablePanel;
class TelemetryPanel;

// Top-level widget of the "Runtime" tab: link-status header, graph-layout
// preset row, and telemetry plots. The signal table and console live in dock
// widgets on the main window (see GetSignalTable / GetConsole); this widget
// wires them to the plots.
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

private slots:
    void OnStoreChanged();
    void OnSavePreset();
    void OnLoadPreset();
    void OnExportSession();
    void OnClearSession();

private:
    void RefreshRecentCombo();

    RuntimeController* controller_;

    QLabel* headerLabel_ = nullptr;
    QLabel* exportStatus_ = nullptr;
    QLineEdit* presetNameEdit_ = nullptr;
    QComboBox* recentCombo_ = nullptr;
    QLabel* presetStatus_ = nullptr;
    TelemetryPanel* telemetryPanel_ = nullptr;
    SignalTablePanel* signalTablePanel_ = nullptr;
    ConsolePanel* consolePanel_ = nullptr;
};

}  // namespace NodeGUI::runtime
