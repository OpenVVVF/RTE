#include "ScenarioFile.h"

#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>

namespace NodeGUI::simulation {

namespace {

// Reads <object>[key] as a double when it is a JSON number, else *fallback.
double ReadDouble(const QJsonObject& object, const QString& key, double fallback) {
    const QJsonValue value = object.value(key);
    return value.isDouble() ? value.toDouble() : fallback;
}

}  // namespace

bool ScenarioFile::LoadFromFile(const QString& path, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("cannot open %1: %2").arg(path, file.errorString());
        }
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) {
            *error = QStringLiteral("cannot parse %1: %2")
                         .arg(path, parseError.errorString());
        }
        return false;
    }
    path_ = path;
    root_ = document.object();
    return true;
}

bool ScenarioFile::SaveToFile(const QString& path, QString* error) const {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = QStringLiteral("cannot write %1: %2").arg(path, file.errorString());
        }
        return false;
    }
    file.write(QJsonDocument(root_).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error) {
            *error = QStringLiteral("cannot write %1: %2").arg(path, file.errorString());
        }
        return false;
    }
    return true;
}

ScenarioValues ScenarioFile::Values() const {
    ScenarioValues values;
    const QJsonObject motor = root_.value(QStringLiteral("motor")).toObject();
    values.rsOhm = ReadDouble(motor, QStringLiteral("rs_ohm"), values.rsOhm);
    values.ldH = ReadDouble(motor, QStringLiteral("ld_h"), values.ldH);
    values.lqH = ReadDouble(motor, QStringLiteral("lq_h"), values.lqH);
    values.fluxWb = ReadDouble(motor, QStringLiteral("flux_wb"), values.fluxWb);
    values.vdcV = ReadDouble(motor, QStringLiteral("vdc_v"), values.vdcV);
    values.inertiaKgM2 =
        ReadDouble(motor, QStringLiteral("inertia_kg_m2"), values.inertiaKgM2);
    values.frictionNmPerRadS =
        ReadDouble(motor, QStringLiteral("friction_nm_per_rad_s"), values.frictionNmPerRadS);
    const QJsonValue polePairs = motor.value(QStringLiteral("pole_pairs"));
    if (polePairs.isDouble() && polePairs.toDouble() > 0.0) {
        values.polePairs = static_cast<int>(polePairs.toDouble());
    }

    const QJsonObject sim = root_.value(QStringLiteral("simulation")).toObject();
    values.durationS = ReadDouble(sim, QStringLiteral("duration_s"), values.durationS);
    values.timIsrHz = ReadDouble(sim, QStringLiteral("tim_isr_hz"), values.timIsrHz);
    values.appLoopHz = ReadDouble(sim, QStringLiteral("app_loop_hz"), values.appLoopHz);

    const QJsonObject plant = sim.value(QStringLiteral("plant")).toObject();
    const QString backend = plant.value(QStringLiteral("backend")).toString();
    if (!backend.isEmpty()) {
        values.backend = backend;
    }
    values.netlist = plant.value(QStringLiteral("netlist")).toString();
    return values;
}

void ScenarioFile::Apply(const ScenarioValues& values) {
    QJsonObject motor = root_.value(QStringLiteral("motor")).toObject();
    motor[QStringLiteral("rs_ohm")] = values.rsOhm;
    motor[QStringLiteral("ld_h")] = values.ldH;
    motor[QStringLiteral("lq_h")] = values.lqH;
    motor[QStringLiteral("flux_wb")] = values.fluxWb;
    motor[QStringLiteral("pole_pairs")] = values.polePairs;
    motor[QStringLiteral("vdc_v")] = values.vdcV;
    motor[QStringLiteral("inertia_kg_m2")] = values.inertiaKgM2;
    motor[QStringLiteral("friction_nm_per_rad_s")] = values.frictionNmPerRadS;
    root_[QStringLiteral("motor")] = motor;

    QJsonObject sim = root_.value(QStringLiteral("simulation")).toObject();
    sim[QStringLiteral("duration_s")] = values.durationS;
    sim[QStringLiteral("tim_isr_hz")] = values.timIsrHz;
    sim[QStringLiteral("app_loop_hz")] = values.appLoopHz;

    const bool ngspice = values.backend == QStringLiteral("ngspice");
    QJsonObject plant = sim.value(QStringLiteral("plant")).toObject();
    if (ngspice || !plant.isEmpty()) {
        // Only create the plant object when ngspice needs it; an existing one
        // is updated in place and keeps its other keys (e.g. substeps).
        plant[QStringLiteral("backend")] = values.backend;
        if (ngspice) {
            plant[QStringLiteral("netlist")] = values.netlist;
        }
        sim[QStringLiteral("plant")] = plant;
    }
    root_[QStringLiteral("simulation")] = sim;
}

}  // namespace NodeGUI::simulation
