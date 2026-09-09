#pragma once

#include "ScenarioFile.h"

#include <QDialog>
#include <QString>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace NodeGUI::simulation {

// Scenario picker + small form editor for "Build & Run Simulation". Lists the
// scenarios found by SimRunner::FindScenariosForGraph plus a "default (auto)"
// entry that defers to rte's automatic selection. The form edits the common
// motor/simulation/plant keys of the selected file; Save / Save As write the
// file through ScenarioFile, which preserves unknown keys but not key order
// or comments. Run accepts the dialog with the current selection.
class ScenarioDialog : public QDialog {
    Q_OBJECT

public:
    // editOnly hides the Run button (plain "Scenario Editor" usage).
    ScenarioDialog(const QString& graphPath, bool editOnly, QWidget* parent = nullptr);

    // The scenario to pass on the command line; empty = default (auto).
    QString SelectedScenarioPath() const;
    bool RunRequested() const { return runRequested_; }

protected:
    void done(int result) override;

private:
    void PopulateScenarios();
    void LoadSelection();
    void Fill(const ScenarioValues& values);
    ScenarioValues Gather() const;
    bool SaveTo(const QString& path);
    void OnBackendChanged();
    void SelectPath(const QString& path);

    QString graphPath_;
    bool runRequested_ = false;
    ScenarioFile file_;
    // Where Save writes for the current selection; "" means the current
    // selection has no file yet (Save falls through to Save As).
    QString loadedPath_;

    QComboBox* scenarioCombo_ = nullptr;
    QLabel* selectionNote_ = nullptr;
    QDoubleSpinBox* rsOhm_ = nullptr;
    QDoubleSpinBox* ldH_ = nullptr;
    QDoubleSpinBox* lqH_ = nullptr;
    QDoubleSpinBox* fluxWb_ = nullptr;
    QSpinBox* polePairs_ = nullptr;
    QDoubleSpinBox* vdcV_ = nullptr;
    QDoubleSpinBox* inertia_ = nullptr;
    QDoubleSpinBox* friction_ = nullptr;
    QDoubleSpinBox* duration_ = nullptr;
    QDoubleSpinBox* timIsrHz_ = nullptr;
    QDoubleSpinBox* appLoopHz_ = nullptr;
    QComboBox* backend_ = nullptr;
    QLineEdit* netlist_ = nullptr;
    QPushButton* saveButton_ = nullptr;
    QPushButton* saveAsButton_ = nullptr;
    QPushButton* runButton_ = nullptr;
};

}  // namespace NodeGUI::simulation
