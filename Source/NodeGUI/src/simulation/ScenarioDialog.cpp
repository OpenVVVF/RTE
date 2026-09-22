#include "ScenarioDialog.h"

#include "SimRunner.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>

namespace NodeGUI::simulation {

namespace {

constexpr char kLastScenarioKey[] = "simulation/lastScenario";

QDoubleSpinBox* MakeDoubleSpin(double min, double max, int decimals, double step,
                               const QString& suffix, QWidget* parent) {
    auto* spin = new QDoubleSpinBox(parent);
    spin->setRange(min, max);
    spin->setDecimals(decimals);
    spin->setSingleStep(step);
    if (!suffix.isEmpty()) {
        spin->setSuffix(QStringLiteral(" ") + suffix);
    }
    return spin;
}

}  // namespace

ScenarioDialog::ScenarioDialog(const QString& graphPath, bool editOnly, QWidget* parent)
    : QDialog(parent)
    , graphPath_(graphPath) {
    setWindowTitle(editOnly ? QStringLiteral("Simulation Scenario Editor")
                            : QStringLiteral("Build & Run Simulation"));
    setMinimumWidth(460);

    auto* layout = new QVBoxLayout(this);
    auto* reliabilityWarning = new QLabel(
        QStringLiteral("Warning: simulation is not correctly implemented and is not advised for control or hardware decisions."), this);
    reliabilityWarning->setWordWrap(true);
    reliabilityWarning->setStyleSheet(QStringLiteral("color: #e09522; font-weight: 600;"));
    layout->addWidget(reliabilityWarning);

    auto* pickerRow = new QHBoxLayout();
    pickerRow->addWidget(new QLabel(QStringLiteral("Scenario:"), this));
    scenarioCombo_ = new QComboBox(this);
    scenarioCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    pickerRow->addWidget(scenarioCombo_, 1);
    layout->addLayout(pickerRow);

    selectionNote_ = new QLabel(this);
    selectionNote_->setWordWrap(true);
    layout->addWidget(selectionNote_);

    auto* form = new QFormLayout();
    rsOhm_ = MakeDoubleSpin(0.0, 1e3, 6, 0.01, QStringLiteral("ohm"), this);
    form->addRow(QStringLiteral("Winding resistance rs"), rsOhm_);
    ldH_ = MakeDoubleSpin(0.0, 10.0, 8, 1e-4, QStringLiteral("H"), this);
    form->addRow(QStringLiteral("d-axis inductance ld"), ldH_);
    lqH_ = MakeDoubleSpin(0.0, 10.0, 8, 1e-4, QStringLiteral("H"), this);
    form->addRow(QStringLiteral("q-axis inductance lq"), lqH_);
    fluxWb_ = MakeDoubleSpin(0.0, 10.0, 6, 1e-3, QStringLiteral("Wb"), this);
    form->addRow(QStringLiteral("PM flux linkage"), fluxWb_);
    polePairs_ = new QSpinBox(this);
    polePairs_->setRange(1, 64);
    form->addRow(QStringLiteral("Pole pairs"), polePairs_);
    vdcV_ = MakeDoubleSpin(0.0, 2000.0, 2, 1.0, QStringLiteral("V"), this);
    form->addRow(QStringLiteral("DC bus voltage"), vdcV_);
    inertia_ = MakeDoubleSpin(0.0, 100.0, 8, 1e-5, QStringLiteral("kg m^2"), this);
    form->addRow(QStringLiteral("Rotor inertia"), inertia_);
    friction_ = MakeDoubleSpin(0.0, 100.0, 8, 1e-4, QStringLiteral("N m s"), this);
    form->addRow(QStringLiteral("Viscous friction"), friction_);

    duration_ = MakeDoubleSpin(0.0, 1e9, 3, 0.1, QStringLiteral("s"), this);
    form->addRow(QStringLiteral("Sim duration"), duration_);
    timIsrHz_ = MakeDoubleSpin(100.0, 1e6, 0, 1000.0, QStringLiteral("Hz"), this);
    form->addRow(QStringLiteral("PWM ISR rate"), timIsrHz_);
    appLoopHz_ = MakeDoubleSpin(10.0, 1e5, 0, 100.0, QStringLiteral("Hz"), this);
    form->addRow(QStringLiteral("App loop rate"), appLoopHz_);

    backend_ = new QComboBox(this);
    backend_->addItem(QStringLiteral("ode"), QStringLiteral("ode"));
    backend_->addItem(QStringLiteral("ngspice"), QStringLiteral("ngspice"));
    form->addRow(QStringLiteral("Plant backend"), backend_);
    netlist_ = new QLineEdit(this);
    netlist_->setPlaceholderText(QStringLiteral("plants/inverter_rl.cir"));
    form->addRow(QStringLiteral("ngspice netlist"), netlist_);
    layout->addLayout(form);

    auto* savingNote = new QLabel(
        QStringLiteral("Saving rewrites only the fields above inside the scenario file; "
                       "all other keys are preserved, but key order and comments are not."),
        this);
    savingNote->setWordWrap(true);
    layout->addWidget(savingNote);

    auto* buttons = new QHBoxLayout();
    saveButton_ = new QPushButton(QStringLiteral("Save"), this);
    buttons->addWidget(saveButton_);
    saveAsButton_ = new QPushButton(QStringLiteral("Save As..."), this);
    buttons->addWidget(saveAsButton_);
    buttons->addStretch(1);
    runButton_ = new QPushButton(QStringLiteral("Run"), this);
    runButton_->setDefault(true);
    buttons->addWidget(runButton_);
    if (editOnly) {
        runButton_->hide();
    }
    auto* closeButton =
        new QPushButton(editOnly ? QStringLiteral("Close") : QStringLiteral("Cancel"), this);
    buttons->addWidget(closeButton);
    layout->addLayout(buttons);

    connect(scenarioCombo_, &QComboBox::currentIndexChanged,
            this, [this](int) { LoadSelection(); });
    connect(backend_, &QComboBox::currentIndexChanged,
            this, [this](int) { OnBackendChanged(); });
    connect(saveButton_, &QPushButton::clicked, this, [this] {
        if (loadedPath_.isEmpty()) {
            saveAsButton_->click();
            return;
        }
        SaveTo(loadedPath_);
    });
    connect(saveAsButton_, &QPushButton::clicked, this, [this] {
        const QStringList dirs = SimRunner::ScenarioDirsForGraph(graphPath_);
        const QString startDir = !dirs.isEmpty()
            ? dirs.first()
            : QFileInfo(graphPath_).absolutePath();
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("Save Scenario As"), startDir,
            QStringLiteral("JSON (*.json)"));
        if (path.isEmpty()) {
            return;
        }
        if (SaveTo(path)) {
            SelectPath(path);
        }
    });
    connect(runButton_, &QPushButton::clicked, this, [this] {
        if (backend_->currentData().toString() == QStringLiteral("ngspice")
            && netlist_->text().trimmed().isEmpty()) {
            selectionNote_->setText(
                QStringLiteral("The ngspice backend needs a netlist path (or pick ode)."));
            return;
        }
        // rte reads the scenario file from disk: persist unsaved edits so the
        // run uses exactly what the form shows. Without a loaded file there
        // is nowhere to save (auto selection with no resolvable file).
        if (dirty_ && !loadedPath_.isEmpty() && !SaveTo(loadedPath_)) {
            return;  // note already reports the failure; keep the dialog open
        }
        runRequested_ = true;
        accept();
    });
    connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);

    // Track user edits to the form so Run can persist them before starting.
    for (QDoubleSpinBox* spin : {rsOhm_, ldH_, lqH_, fluxWb_, vdcV_, inertia_,
                                 friction_, duration_, timIsrHz_, appLoopHz_}) {
        connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, [this](double) { MarkDirty(); });
    }
    connect(polePairs_, qOverload<int>(&QSpinBox::valueChanged),
            this, [this](int) { MarkDirty(); });
    connect(backend_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) { MarkDirty(); });
    connect(netlist_, &QLineEdit::textChanged,
            this, [this](const QString&) { MarkDirty(); });

    // Save the side the previous run used.
    PopulateScenarios();
    const QString last =
        QSettings(QStringLiteral("RTE"), QStringLiteral("RTEStudio"))
            .value(QString::fromLatin1(kLastScenarioKey))
            .toString();
    for (int i = 0; i < scenarioCombo_->count(); ++i) {
        if (scenarioCombo_->itemData(i).toString() == last) {
            scenarioCombo_->setCurrentIndex(i);
            break;
        }
    }
    LoadSelection();
}

QString ScenarioDialog::SelectedScenarioPath() const {
    return scenarioCombo_->currentData().toString();
}

void ScenarioDialog::done(int result) {
    // Remember the pick regardless of how the dialog closes so the next run
    // starts from it.
    if (scenarioCombo_) {
        QSettings(QStringLiteral("RTE"), QStringLiteral("RTEStudio"))
            .setValue(QString::fromLatin1(kLastScenarioKey), SelectedScenarioPath());
    }
    QDialog::done(result);
}

void ScenarioDialog::PopulateScenarios() {
    scenarioCombo_->blockSignals(true);
    scenarioCombo_->clear();
    scenarioCombo_->addItem(QStringLiteral("default (auto)"), QString{});
    const QStringList scenarios = SimRunner::FindScenariosForGraph(graphPath_);
    const QString autoPath = SimRunner::AutoScenarioForGraph(graphPath_);
    for (const QString& path : scenarios) {
        const QFileInfo info(path);
        QString label = info.fileName();
        if (path == autoPath) {
            label += QStringLiteral("  (auto for this graph)");
        }
        scenarioCombo_->addItem(label, path);
        scenarioCombo_->setItemData(scenarioCombo_->count() - 1,
                                    info.absoluteFilePath(), Qt::ToolTipRole);
    }
    scenarioCombo_->blockSignals(false);
}

void ScenarioDialog::SelectPath(const QString& path) {
    for (int i = 0; i < scenarioCombo_->count(); ++i) {
        if (scenarioCombo_->itemData(i).toString() == path) {
            scenarioCombo_->setCurrentIndex(i);
            return;
        }
    }
    PopulateScenarios();
    for (int i = 0; i < scenarioCombo_->count(); ++i) {
        if (scenarioCombo_->itemData(i).toString() == path) {
            scenarioCombo_->setCurrentIndex(i);
            return;
        }
    }
}

void ScenarioDialog::LoadSelection() {
    const QString selection = scenarioCombo_->currentData().toString();
    QString path = selection;
    if (path.isEmpty()) {
        // Mirror what rte would pick so the form previews something real.
        path = SimRunner::AutoScenarioForGraph(graphPath_);
    }
    loadedPath_ = path;

    QString loadError;
    if (!path.isEmpty() && file_.LoadFromFile(path, &loadError)) {
        selectionNote_->setText(selection.isEmpty()
            ? QStringLiteral("(auto) rte resolves this run to %1").arg(path)
            : path);
    } else {
        file_ = ScenarioFile{};
        if (path.isEmpty()) {
            loadedPath_.clear();
            selectionNote_->setText(QStringLiteral(
                "(auto) no scenario file found next to HostSim; rte will report an error. "
                "Editing shows defaults."));
        } else {
            selectionNote_->setText(QStringLiteral("%1\nCould not load: %2 (editing defaults)")
                                        .arg(path, loadError));
        }
    }
    Fill(file_.Values());
    OnBackendChanged();
    dirty_ = false;
}

void ScenarioDialog::Fill(const ScenarioValues& values) {
    fillingForm_ = true;
    rsOhm_->setValue(values.rsOhm);
    ldH_->setValue(values.ldH);
    lqH_->setValue(values.lqH);
    fluxWb_->setValue(values.fluxWb);
    polePairs_->setValue(values.polePairs);
    vdcV_->setValue(values.vdcV);
    inertia_->setValue(values.inertiaKgM2);
    friction_->setValue(values.frictionNmPerRadS);
    duration_->setValue(values.durationS);
    timIsrHz_->setValue(values.timIsrHz);
    appLoopHz_->setValue(values.appLoopHz);
    const int backendIndex = backend_->findData(values.backend);
    backend_->setCurrentIndex(backendIndex >= 0 ? backendIndex : 0);
    netlist_->setText(values.netlist);
    fillingForm_ = false;
}

ScenarioValues ScenarioDialog::Gather() const {
    ScenarioValues values;
    values.rsOhm = rsOhm_->value();
    values.ldH = ldH_->value();
    values.lqH = lqH_->value();
    values.fluxWb = fluxWb_->value();
    values.polePairs = polePairs_->value();
    values.vdcV = vdcV_->value();
    values.inertiaKgM2 = inertia_->value();
    values.frictionNmPerRadS = friction_->value();
    values.durationS = duration_->value();
    values.timIsrHz = timIsrHz_->value();
    values.appLoopHz = appLoopHz_->value();
    values.backend = backend_->currentData().toString();
    values.netlist = netlist_->text().trimmed();
    return values;
}

bool ScenarioDialog::SaveTo(const QString& path) {
    file_.Apply(Gather());
    QString error;
    if (!file_.SaveToFile(path, &error)) {
        selectionNote_->setText(QStringLiteral("Save failed: %1").arg(error));
        return false;
    }
    loadedPath_ = path;
    dirty_ = false;
    selectionNote_->setText(QStringLiteral("Saved %1").arg(path));
    return true;
}

void ScenarioDialog::OnBackendChanged() {
    netlist_->setEnabled(backend_->currentData().toString() == QStringLiteral("ngspice"));
}

void ScenarioDialog::MarkDirty() {
    if (!fillingForm_) {
        dirty_ = true;
    }
}

}  // namespace NodeGUI::simulation
