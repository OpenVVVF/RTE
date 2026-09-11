#pragma once

#include <QJsonObject>
#include <QString>

namespace NodeGUI::simulation {

// Values the GUI form edits in a HostSim scenario file. Defaults mirror
// Images/HostSim/scenarios/default_motor.json.
struct ScenarioValues {
    double rsOhm = 0.08;
    double ldH = 1.2e-4;
    double lqH = 1.2e-4;
    double fluxWb = 8.5e-3;
    int polePairs = 7;
    double vdcV = 48.0;
    double inertiaKgM2 = 1.2e-5;
    double frictionNmPerRadS = 2.0e-4;
    double durationS = 0.5;
    double timIsrHz = 10000.0;
    double appLoopHz = 1000.0;
    QString backend = QStringLiteral("ode");  // "ode" or "ngspice"
    QString netlist;                          // ngspice only, e.g. plants/inverter_rl.cir
};

// Typed view over the known keys of a HostSim scenario JSON document. Load
// keeps the whole object; Apply rewrites only the known keys (creating the
// motor/simulation objects when absent), so unknown keys survive a save
// round-trip. Key order and comments are NOT preserved — the file is
// rewritten from QJsonDocument.
class ScenarioFile {
public:
    bool LoadFromFile(const QString& path, QString* error);
    bool SaveToFile(const QString& path, QString* error) const;

    ScenarioValues Values() const;
    void Apply(const ScenarioValues& values);

    QString Path() const { return path_; }

private:
    QString path_;
    QJsonObject root_;
};

}  // namespace NodeGUI::simulation
