#pragma once

#include <QString>

#include <cmath>

namespace NodeGUI::runtime {

// Unit metadata for a telemetry key.
//
// HostSim publishes lowercase keys (duty_u, i_a, omega_e, vdc_v, ...) while the
// legacy firmware used uppercase ones (V_bus, I_ROTOR_speed). Both are matched
// here, legacy first, because "I_ROTOR_speed" is a rotor angle rather than amps.
struct SignalUnit {
    QString axisLabel;  // full label shown on a plot title strip
    QString suffix;     // short suffix for a table cell, may be empty
};

inline SignalUnit UnitInfoForSignal(const QString& name) {
    if (name.contains(QStringLiteral("ROTOR"))) {
        return {QStringLiteral("Degrees (deg)"), QStringLiteral("deg")};
    }
    if (name.contains(QStringLiteral("RATE"))) {
        return {QStringLiteral("kHz"), QStringLiteral("kHz")};
    }
    if (name.startsWith(QStringLiteral("pwm_gate_"))) {
        return {QStringLiteral("Gate (0/1)"), QString()};
    }
    if (name.startsWith(QStringLiteral("pwm_v_"))) {
        return {QStringLiteral("Volts (V)"), QStringLiteral("V")};
    }
    if (name.startsWith(QStringLiteral("duty_"))) {
        return {QStringLiteral("Duty (%)"), QStringLiteral("%")};
    }
    if (name.startsWith(QStringLiteral("omega_"))) {
        return {QStringLiteral("Angular rate (rad/s)"), QStringLiteral("rad/s")};
    }
    if (name.startsWith(QStringLiteral("theta_")) || name.endsWith(QStringLiteral("_deg"))) {
        return {QStringLiteral("Degrees (deg)"), QStringLiteral("deg")};
    }
    if (name.endsWith(QStringLiteral("_hz"))) {
        return {QStringLiteral("Hertz (Hz)"), QStringLiteral("Hz")};
    }
    // Checked before the amps rule below, which would otherwise claim
    // "throttle_a" on its trailing "_a".
    if (name.startsWith(QStringLiteral("throttle_")) ||
        name == QStringLiteral("mod_index")) {
        return {QStringLiteral("Normalized (0..1)"), QString()};
    }
    if (name == QStringLiteral("sim_speed")) {
        return {QStringLiteral("Speed (x realtime)"), QStringLiteral("x")};
    }
    if (name.startsWith(QStringLiteral("vdc")) || name.startsWith(QStringLiteral("V_")) ||
        name.endsWith(QStringLiteral("_v"))) {
        return {QStringLiteral("Volts (V)"), QStringLiteral("V")};
    }
    if (name.startsWith(QStringLiteral("i_")) || name.startsWith(QStringLiteral("I_")) ||
        name.endsWith(QStringLiteral("_a"))) {
        return {QStringLiteral("Amps (A)"), QStringLiteral("A")};
    }
    return {QStringLiteral("Value"), QString()};
}

inline QString UnitForSignal(const QString& name) {
    return UnitInfoForSignal(name).axisLabel;
}

inline QString UnitSuffixForSignal(const QString& name) {
    return UnitInfoForSignal(name).suffix;
}

// Roughly three significant figures. Six fixed decimals turns a 221 A reading
// into four digits of noise and makes the value column impossible to scan.
inline QString FormatSignalValue(float value) {
    const double v = static_cast<double>(value);
    const double mag = std::fabs(v);
    if (mag != 0.0 && (mag >= 1e5 || mag < 1e-3)) {
        return QString::number(v, 'e', 2);
    }
    int decimals = 3;
    if (mag >= 100.0) {
        decimals = 0;
    } else if (mag >= 10.0) {
        decimals = 1;
    } else if (mag >= 1.0) {
        decimals = 2;
    }
    return QString::number(v, 'f', decimals);
}

}  // namespace NodeGUI::runtime
