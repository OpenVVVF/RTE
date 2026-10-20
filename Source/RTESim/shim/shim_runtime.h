#pragma once

/**
 * Host-side runtime behind the firmware platform_api: binds the platform
 * shim to the generated plant state, the simulated clock, the FRAM-seeded
 * config store, and the latest telemetry values logged by generated
 * telemetry nodes.
 *
 * Ported from twin/native/src/shim_runtime.h; the plant handle is now the
 * PlantCodegen contract (plant::PlantState / plant::PlantMeas) instead of
 * the monolithic twin::PlantPmsm, and the measurement view is a snapshot
 * struct copied once per control period by the scheduler.
 *
 * This header only forward-declares the plant types so the scheduler and
 * shim can share it without both including plant_generated.h.
 */

#include <map>
#include <string>

namespace plant {
struct PlantState;
struct PlantMeas;
}

namespace rtesim_host {

class ShimRuntime {
public:
    static ShimRuntime& instance() {
        static ShimRuntime s;
        return s;
    }

    plant::PlantState* plant = nullptr;
    const plant::PlantMeas* meas = nullptr;  // scheduler-owned snapshot, period-fresh
    unsigned long long now_us = 0;
    float domain_dt = 0.0f;

    // FRAM-like key/value config store (platform_config_*). Seeded by main
    // with the same values the python twin's FocController believes, so the
    // generated graph and the python replica run identical parameters.
    std::map<std::string, float> config;

    // Latest value per key from platform_telemetry_log_f32.
    std::map<std::string, float> telemetry;

    // Latest raw PWM duties written through platform_pwm_set.
    float pwm_du = 50.0f;
    float pwm_dv = 50.0f;
    float pwm_dw = 50.0f;
};

} // namespace rtesim_host
