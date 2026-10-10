#include "Inverter/Drivers/Sensors/ApplicationSensors.h"
#include "Inverter/Control/FaultManager.h"
#include <cmath>

namespace Inverter {

float ApplicationSensors::throttleAVoltage() const {
    return m_thr_a_v;
}

float ApplicationSensors::throttleBVoltage() const {
    return m_thr_b_v;
}

float ApplicationSensors::throttleA() const {
    return m_thr_a_norm;
}

float ApplicationSensors::throttleB() const {
    return m_thr_b_enabled ? m_thr_b_norm : 0.0f;
}

bool ApplicationSensors::throttleBEnabled() const {
    return m_thr_b_enabled;
}

bool ApplicationSensors::throttlePlausible() const {
    return m_thr_plausible;
}

void ApplicationSensors::updateThrottlePlausibility(uint32_t now_ms) {
    const float diff = std::fabs(m_thr_a_cand - m_thr_b_cand);
    if (!m_thr_b_enabled || diff <= THROTTLE_PLAUS_TOL) {
        m_thr_plausible = true;
        m_thr_implausible_since = 0;
        m_thr_fault_raised = false;  /**< re-arm: a cleared fault can re-raise */
        return;
    }
    if (m_thr_implausible_since == 0) {
        m_thr_implausible_since = now_ms;
        return;
    }
    if ((now_ms - m_thr_implausible_since) >= THROTTLE_PLAUS_MS) {
        m_thr_plausible = false;
        if (!m_thr_fault_raised ||
            !FaultManager::instance().isActive(FaultSource::ThrottlePlausibility)) {
            m_thr_fault_raised = true;
            FaultManager::instance().raise(FaultSource::ThrottlePlausibility,
                                           FaultReason::ThrottlePlausibilityMismatch);
        }
    }
}

} // namespace Inverter
