#pragma once

#include <QWidget>

#include <QStringList>

#include <array>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

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
    static std::array<QStringList, 3> BuiltinSpwmLayout();
    static std::array<QStringList, 3> BuiltinFocLayout();
    void EnsureBuiltinPresets();
    void ApplyLayoutIfEmpty(const std::array<QStringList, 3>& layout);
    void ApplySpwmViewWindows();
    void ApplyFocViewWindows();

public slots:
    void OnSaveFramKeys();
    void OnLoadFramKeys();

private slots:
    void OnStoreChanged();
    void OnSavePreset();
    void OnLoadPreset();
    void OnExportSession();
    void OnClearSession();
    void OnLoadBuiltinSpwm();
    void OnLoadBuiltinFoc();
    void OnTogglePause();
    void OnSimPauseChanged(bool paused);

private:
    void RefreshRecentCombo();
    void UpdatePauseButton(bool paused);

    bool applied_builtin_layout_ = false;

    RuntimeController* controller_;
    FramKeysManager* framKeysManager_ = nullptr;

    QLabel* exportStatus_ = nullptr;
    // Compact status chips replacing the old single-line wall of counters.
    QLabel* linkChip_ = nullptr;
    QLabel* stateChip_ = nullptr;
    QLabel* rateChip_ = nullptr;
    QLabel* healthChip_ = nullptr;
    QPushButton* pauseButton_ = nullptr;
    QLineEdit* presetNameEdit_ = nullptr;
    QComboBox* recentCombo_ = nullptr;
    QLabel* presetStatus_ = nullptr;
    TelemetryPanel* telemetryPanel_ = nullptr;
    SignalTablePanel* signalTablePanel_ = nullptr;
    ConsolePanel* consolePanel_ = nullptr;
};

}  // namespace NodeGUI::runtime
