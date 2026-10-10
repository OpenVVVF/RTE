#include "Inverter/Control/DcLinkCommandLimits.h"

#include <cassert>
#include <limits>

int main() {
    using Inverter::limitDcLinkIq;
    // OV blocks braking in either direction and preserves either motoring sign.
    assert(limitDcLinkIq(50.0f, 500.0f, true, 0.0f) == 50.0f);
    assert(limitDcLinkIq(-50.0f, -500.0f, true, 0.0f) == -50.0f);
    assert(limitDcLinkIq(-50.0f, 500.0f, true, 0.0f) == 0.0f);
    assert(limitDcLinkIq(50.0f, -500.0f, true, 0.0f) == 0.0f);
    // No OV condition means both torque directions are available.
    assert(limitDcLinkIq(-50.0f, 500.0f, false, 0.0f) == -50.0f);
    assert(limitDcLinkIq(50.0f, -500.0f, false, 0.0f) == 50.0f);
    // Standstill and encoder noise cannot prevent starting either direction.
    assert(limitDcLinkIq(50.0f, 0.0f, true, 0.0f) == 50.0f);
    assert(limitDcLinkIq(-50.0f, 1.0f, true, 0.0f) == -50.0f);
    assert(limitDcLinkIq(50.0f, -1.0f, true, 0.0f) == 50.0f);
    // UV derates both motoring and braking; OV still takes precedence on regen.
    assert(limitDcLinkIq(50.0f, 500.0f, false, 10.0f) == 10.0f);
    assert(limitDcLinkIq(-50.0f, 500.0f, false, 10.0f) == -10.0f);
    assert(limitDcLinkIq(-5.0f, 500.0f, false, 10.0f) == -5.0f);
    assert(limitDcLinkIq(50.0f, 500.0f, true, 10.0f) == 10.0f);
    assert(limitDcLinkIq(-50.0f, 500.0f, true, 10.0f) == 0.0f);
    // Nonfinite torque or unknown direction during OV cannot reach control.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    assert(limitDcLinkIq(nan, 500.0f, false, 0.0f) == 0.0f);
    assert(limitDcLinkIq(inf, 500.0f, false, 0.0f) == 0.0f);
    assert(limitDcLinkIq(50.0f, nan, true, 0.0f) == 0.0f);
    assert(limitDcLinkIq(50.0f, inf, true, 0.0f) == 0.0f);
}
