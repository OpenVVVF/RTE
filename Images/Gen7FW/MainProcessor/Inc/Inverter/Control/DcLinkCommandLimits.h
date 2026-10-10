#pragma once

#include <algorithm>
#include <cmath>

namespace Inverter {

/* Iq torque follows the sign-corrected electrical rotor angle. Regeneration
 * opposes rotation; either Iq sign can motor or regenerate. A small electrical
 * speed deadband avoids interpreting stationary encoder noise as rotation. */
inline float limitDcLinkIq(float iq_a, float electrical_rpm,
                           bool disable_regen, float derate_limit_a) {
    if (!std::isfinite(iq_a)) return 0.0f;
    if (disable_regen) {
        if (!std::isfinite(electrical_rpm)) return 0.0f;
        constexpr float STATIONARY_ELECTRICAL_RPM = 5.0f;
        const bool regenerating =
            (electrical_rpm > STATIONARY_ELECTRICAL_RPM && iq_a < 0.0f) ||
            (electrical_rpm < -STATIONARY_ELECTRICAL_RPM && iq_a > 0.0f);
        if (regenerating) iq_a = 0.0f;
    }
    if (std::isfinite(derate_limit_a) && derate_limit_a > 0.0f) {
        iq_a = std::clamp(iq_a, -derate_limit_a, derate_limit_a);
    }
    return iq_a;
}

} // namespace Inverter
