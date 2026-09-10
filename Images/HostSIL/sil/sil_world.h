/*
 * sil_world.h — shared simulation state for HostSIL.
 *
 * One SilWorld instance owns the PMSM plant (HostSim's OdePlant/MotorModel)
 * plus the small amount of "physical world" state the firmware sensor shims
 * read: DC-link voltage, applied pole voltages, DC-link current estimate and
 * the throttle pin voltages from the scenario.
 *
 * Concurrency: written by the SIL scheduler, read by the firmware shims —
 * but the two contexts are strictly exclusive (see sil_rt.h), so the fields
 * are plain values, no atomics.
 */
#ifndef SIL_WORLD_H
#define SIL_WORLD_H

#include "plant/ode_plant.h"

#include <cmath>
#include <cstdint>

struct SilWorld {
    hostsim::OdePlant plant;

    /* Scenario constants (set once before boot). */
    float vdc_v         = 48.0f;
    float ambient_temp_c = 25.0f;

    /* Latest inverter pole voltages [V] (duty * vdc when driving, else 0). */
    float phase_pole_v[3] = {0.0f, 0.0f, 0.0f};

    /* Estimated DC-link current [A] from instantaneous power balance. */
    float dc_link_current_a = 0.0f;

    /* Throttle input pin voltages [V] (from the scenario profiles). */
    float throttle_a_v = 0.0f;
    float throttle_b_v = 0.0f;

    /* Encoder trigger select: false = free-running (TIM2, 10 kHz), true =
     * synchronized to the TIM1 update event (set by
     * EncoderADC::useSynchronizedTrigger). */
    bool encoder_sync_trigger = false;

    /* Set by EncoderADC::start(); the scheduler only feeds the sin/cos
     * stream while the firmware has the channel running. */
    bool encoder_stream_running = false;

    /* --- Fault-injection state (scenario "faults" block, applied by the
     * scheduler in main.cpp; read by the shims).  Defaults inject nothing. */
    /* Phase-current spike added at the ADC counts level in
     * sil_phase_current_adc.cpp (oc_fault_phase: 0=U, 1=V, 2=W). */
    bool  oc_fault_active = false;
    int   oc_fault_phase = 0;
    float oc_fault_a = 0.0f;

    /* Encoder stream faults (sil_encoder_adc.cpp): frozen = no new samples
     * (staleness), sig_lost = sin/cos pinned at the bias mid (amplitude
     * collapse). */
    bool encoder_frozen = false;
    bool encoder_sig_lost = false;

    /* Scenario-driven temperature channels [degC] (sil_app_sensors.cpp);
     * NaN = channel not modeled (application reports NAN, as when the
     * sensor is not populated).  0..2 = board, 3 = motor. */
    float temp_c[4] = {NAN, NAN, NAN, NAN};
};

SilWorld& silWorld();

#endif /* SIL_WORLD_H */
