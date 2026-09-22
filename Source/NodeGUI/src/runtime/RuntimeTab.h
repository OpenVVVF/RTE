#pragma once

#include <QWidget>

#include <array>
#include <QStringList>

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

public slots:
    void OnSaveFramKeys();
    void OnLoadFramKeys();

private slots:
    void OnStoreChanged();
    void OnSavePreset();
    void OnLoadPreset();
    void OnLoadBuiltinSpwm();
    void OnLoadBuiltinFoc();
    void OnExportSession();
    void OnClearSession();

private:
    // Built-in demo layouts; also installed as persisted presets so they can
    // be tweaked by the user.
    static std::array<QStringList, 3> BuiltinSpwmLayout();
    static std::array<QStringList, 3> BuiltinFocLayout();
    static std::array<QStringList, 3> BuiltinNativeFocLayout();
    void EnsureBuiltinPresets();
    void ApplyLayoutIfEmpty(const std::array<QStringList, 3>& layout);
    void ApplySpwmViewWindows();
    void ApplyFocViewWindows();

    void RefreshRecentCombo();

    RuntimeController* controller_;
    FramKeysManager* framKeysManager_ = nullptr;
    bool applied_builtin_layout_ = false;

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
