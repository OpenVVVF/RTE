#pragma once

#include <algorithm>
#include <cmath>

// Hardware-independent math, shared by graph control, native voltage PWM and tests.
namespace Inverter::CurrentLoopMath {
struct Vector { float d = 0.0f, q = 0.0f; };
struct Duties { float a = 50.0f, b = 50.0f, c = 50.0f; };
inline bool finite(Vector v) { return std::isfinite(v.d) && std::isfinite(v.q); }
inline Vector midpointPair(Vector current, Vector previous, bool opposite,
                           bool consecutive, float interval, float pwmPeriod) {
    if (opposite && consecutive && finite(current) && finite(previous) &&
        interval > 0 && interval < 0.75f * pwmPeriod)
        return {0.5f * (current.d + previous.d), 0.5f * (current.q + previous.q)};
    return current;
}
inline Vector midpointCycle(Vector current, Vector previous, Vector older,
                            bool coherentCycle) {
    if (!coherentCycle || !finite(current) || !finite(previous) || !finite(older)) return current;
    return {0.25f * current.d + 0.5f * previous.d + 0.25f * older.d,
            0.25f * current.q + 0.5f * previous.q + 0.25f * older.q};
}
inline Vector limit(Vector v, float maxV) {
    if (!finite(v) || !std::isfinite(maxV) || maxV <= 0.0f) return {};
    const float mag = std::hypot(v.d, v.q);
    if (mag > maxV) { const float s = maxV / mag; v.d *= s; v.q *= s; }
    return v;
}
// Inputs are physical alpha/beta volts; output is high-side duty percent.
inline Duties modulate(Vector ab, float bus) {
    if (!finite(ab) || !std::isfinite(bus) || bus <= 1.0f) return {};
    const float a = ab.d;
    const float b = -0.5f * ab.d + 0.866025403784f * ab.q;
    const float c = -0.5f * ab.d - 0.866025403784f * ab.q;
    const float common = 0.5f * (std::max({a,b,c}) + std::min({a,b,c}));
    return {std::clamp(50.0f + 100.0f * (a-common)/bus, 0.0f, 100.0f),
            std::clamp(50.0f + 100.0f * (b-common)/bus, 0.0f, 100.0f),
            std::clamp(50.0f + 100.0f * (c-common)/bus, 0.0f, 100.0f)};
}
struct Result { Vector requested, limited, integral; float scale = 1.0f; };
struct Controller {
    Vector integral;
    void reset() { integral = {}; }
    Result step(Vector error, Vector ff, Vector kp, Vector ki, float dt,
                float maxV, bool enabled, float legacyScale = 0.5f) {
        Result r;
        if (!enabled || !finite(error) || !finite(ff) || !finite(kp) || !finite(ki) ||
            !std::isfinite(dt) || dt <= 0.0f || dt > 0.01f ||
            !std::isfinite(maxV) || maxV <= 0.0f) { reset(); return r; }
        integral.d += legacyScale * ki.d * error.d * dt;
        integral.q += legacyScale * ki.q * error.q * dt;
        r.requested = {legacyScale * (kp.d * error.d + ff.d) + integral.d,
                       legacyScale * (kp.q * error.q + ff.q) + integral.q};
        if (!finite(r.requested)) { reset(); return {}; }
        r.limited = limit(r.requested, maxV);
        const float mag = std::hypot(r.requested.d, r.requested.q);
        r.scale = mag > maxV ? maxV / mag : 1.0f;
        // Preserve the old 25/s tracking rate (AwGain/Kp at Kp=.04), independent
        // of the voltage-unit adapter and later proportional-gain changes.
        const float aw = std::min(1.0f, 25.0f * dt);
        integral.d += aw * (r.limited.d - r.requested.d);
        integral.q += aw * (r.limited.q - r.requested.q);
        r.integral = integral;
        return r;
    }
};
} // namespace Inverter::CurrentLoopMath
