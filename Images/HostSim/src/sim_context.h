#pragma once

#include <cstdint>

#include "RteParams.h"

namespace hostsim {

/* Phase-current ADC sensor model configuration (see RteParams.h for what the
 * fields physically mean). Defaults reproduce the ideal Gen6FW signal chain;
 * scenario JSON "adc" keys override individual fields to inject errors. */
struct SimAdcConfig {
    unsigned bits = adc::kBits;                 /* "resolution_bits"        */
    float vref_v = adc::kVrefV;                 /* "vref_v"                 */
    float ref_volts = adc::kRefVolts;           /* "ref_v"                  */
    float divider = adc::kDivider;              /* "divider"                */
    float sensitivity_v_per_a = adc::kSensitivityVPerA; /* "sensitivity_v_per_a" */
    float gain_error = 1.0f;                    /* "gain_error" (multiplier) */
    float offset_u_a = 0.0f;                    /* "offset_u_a" bias, amps   */
    float offset_v_a = 0.0f;                    /* "offset_v_a" bias, amps   */
    float noise_std_a = 0.0f;                   /* "noise_std_a" 1-sigma, A  */
};

struct SimContext {
    float duty_u = 0.0f;
    float duty_v = 0.0f;
    float duty_w = 0.0f;
    /* Duties actually driven into the plant (post live-override); used by the
     * phase-voltage readback for plant backends without terminal state. */
    float duty_applied_u = 0.0f;
    float duty_applied_v = 0.0f;
    float duty_applied_w = 0.0f;
    float throttle_a = 0.0f;
    float throttle_b = 0.0f;
    float vdc_v = 48.0f;        /* DC link as seen by the control code */
    float plant_vdc_v = 48.0f;  /* DC link actually used by the plant   */
    float motor_temp_c = 25.0f;
    float inverter_temp_c = 25.0f;
    bool critical_fault = false;
    bool encoder_sample_new = false;
    /* Set by platform_pwm_set; consumed by the scheduler each tim_isr tick to
     * tell graph-driven duties apart from "nobody is driving the plant". */
    bool pwm_written = false;
    uint64_t time_us = 0;
    /* Incremented after every plant Step(); lets the ADC sample latch tell
     * fresh conversions apart from leftovers of an older plant state. */
    uint64_t plant_step_seq = 0;
};

SimContext& GetSimContext();
void SimNotifyEncoderSample();

class IPlant;
void SimRuntime_RegisterPlant(IPlant* plant);

/* Base-image internal hooks (not part of the graph-facing platform_api.h). */
void SimAdcConfigure(const SimAdcConfig& cfg);
void SimAdcTriggerConversion();
void SimCanSetLoopback(bool enabled);
void SimCanInject(uint8_t bus, uint32_t id, bool ext,
                  const uint8_t* data, uint8_t dlc);
void SimConfigSetBackingFile(const char* path);
void SimConfigPersist();

} // namespace hostsim
