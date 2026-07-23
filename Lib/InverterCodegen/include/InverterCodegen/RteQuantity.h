#pragma once

// Generated firmware uses these quantity aliases so physical units are checked at compile time.
// The unit library underneath is Au (https://github.com/aurora-opensource/au), vendored as a
// single header at Lib/InverterCodegen/third_party/au/au.hh.

#include "au/au.hh"

namespace rte {

// ---------------------------------------------------------------------------
// Scalar quantities
// ---------------------------------------------------------------------------

using Current = au::Quantity<au::Amperes, float>;
using Voltage = au::Quantity<au::Volts, float>;
using Angle = au::Quantity<au::Radians, float>;
using Time = au::Quantity<au::Seconds, float>;
using Frequency = au::Quantity<au::Hertz, float>;

// Torque and angular velocity are composed from base units included in the vendored Au header.
using Torque = au::Quantity<decltype(au::newtons * au::meters)::Unit, float>;
using AngularVelocity = au::Quantity<decltype(au::radians / au::seconds)::Unit, float>;

// Temperature quantities use Celsius as a quantity (not a quantity point).
using Temperature = au::Quantity<au::Celsius, float>;

// Dimensionless values and booleans are kept as plain built-in types to avoid friction in
// control-law code (gains, duty cycles, flags). All physical ports still get Au types.
using Dimensionless = float;
using Boolean = bool;

// ---------------------------------------------------------------------------
// Convenience constructors from plain float values. Codegen emits these so
// parameter literals get the right unit wrapper.
// ---------------------------------------------------------------------------

inline constexpr Current Amperes(float v) { return au::amperes(v); }
inline constexpr Voltage Volts(float v) { return au::volts(v); }
inline constexpr Angle Radians(float v) { return au::radians(v); }
inline constexpr Time Seconds(float v) { return au::seconds(v); }
inline constexpr Frequency Hertz(float v) { return au::hertz(v); }
inline constexpr Temperature Celsius(float v) { return au::celsius_qty(v); }

// Torque and angular velocity are composed from the base units in the vendored Au header.
inline constexpr Torque NewtonMeters(float v) {
    return au::newtons(v) * au::meters(1.0f);
}
inline constexpr AngularVelocity RadiansPerSecond(float v) {
    return au::radians(v) / au::seconds(1.0f);
}

// ---------------------------------------------------------------------------
// Framed quantities for three-phase / rotating-reference-frame math
// ---------------------------------------------------------------------------

struct AbcCurrent {
    Current a;
    Current b;
    Current c;
};

struct AbcVoltage {
    Voltage a;
    Voltage b;
    Voltage c;
};

struct AlphaBetaCurrent {
    Current alpha;
    Current beta;
};

struct AlphaBetaVoltage {
    Voltage alpha;
    Voltage beta;
};

struct DqCurrent {
    Current d;
    Current q;
};

struct DqVoltage {
    Voltage d;
    Voltage q;
};

}  // namespace rte
