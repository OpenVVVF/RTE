#include <cstdint>
#define private public
#include "Inverter/Drivers/Sensors/ApplicationSensors.h"
#undef private
#include "Inverter/Control/FaultManager.h"
#include <cassert>
#include <initializer_list>
#include <limits>

static unsigned raised;
static bool latched;
namespace Inverter {
FaultManager& FaultManager::instance() { static FaultManager f; return f; }
bool FaultManager::isActive(FaultSource source) const {
    assert(source == FaultSource::ThrottlePlausibility);
    return latched;
}
void FaultManager::raise(FaultSource source, FaultReason reason) {
    assert(source == FaultSource::ThrottlePlausibility);
    assert(reason == FaultReason::ThrottlePlausibilityMismatch);
    ++raised;
    latched = true;
}
}
int main() {
    Inverter::ApplicationSensors sensor;
    assert(sensor.throttleBEnabled()); /* Missing config preserves redundancy. */
    sensor.m_thr_a_cand = 1.0f;
    sensor.m_thr_b_cand = 0.0f;
    sensor.updateThrottlePlausibility(10U);
    sensor.updateThrottlePlausibility(109U);
    assert(raised == 0U && sensor.throttlePlausible());
    sensor.updateThrottlePlausibility(110U);
    assert(raised == 1U && latched && !sensor.throttlePlausible());
    sensor.updateThrottlePlausibility(111U);
    assert(raised == 1U);
    latched = false; /* A cleared fault re-raises if mismatch persists. */
    sensor.updateThrottlePlausibility(112U);
    assert(raised == 2U && latched);

    sensor.m_thr_b_enabled = false;
    sensor.m_thr_a_norm = 0.7f;
    sensor.m_thr_b_norm = 0.9f;
    sensor.m_thr_a_v = 2.1071f;
    sensor.m_thr_b_v = 0.3f;
    sensor.updateThrottlePlausibility(113U);
    assert(!sensor.throttleBEnabled() && sensor.throttlePlausible());
    assert(sensor.throttleA() == 0.7f && sensor.throttleB() == 0.0f);
    assert(sensor.throttleAVoltage() == 2.1071f);
    assert(sensor.throttleBVoltage() == 0.3f); /* Raw diagnostic remains. */
    assert(latched); /* Changing mode must not clear a latched safety fault. */
    sensor.m_thr_b_cand = std::numeric_limits<float>::quiet_NaN();
    for (float a : {0.0f, 0.5f, 1.0f}) {
        sensor.m_thr_a_cand = a;
        sensor.updateThrottlePlausibility(1000U);
        assert(sensor.throttlePlausible() && raised == 2U);
    }

    sensor.m_thr_b_enabled = true;
    sensor.m_thr_a_cand = 1.0f;
    sensor.m_thr_b_cand = 0.0f;
    latched = false;
    sensor.updateThrottlePlausibility(2000U);
    sensor.updateThrottlePlausibility(2099U);
    assert(raised == 2U); /* Disabling B reset the old mismatch timer. */
    sensor.updateThrottlePlausibility(2100U);
    assert(raised == 3U && latched && !sensor.throttlePlausible());
    sensor.m_thr_b_cand = 0.95f;
    sensor.updateThrottlePlausibility(2101U);
    assert(sensor.throttlePlausible() && latched);

    sensor.m_thr_b_cand = 0.0f;
    sensor.updateThrottlePlausibility(UINT32_MAX - 50U);
    latched = false;
    sensor.updateThrottlePlausibility(49U);
    assert(raised == 4U); /* Debounce handles millisecond rollover. */
}
