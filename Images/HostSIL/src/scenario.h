/*
 * scenario.h — HostSIL scenario description (mirrors the HostSim JSON
 * schema style).
 */
#ifndef SIL_SCENARIO_H
#define SIL_SCENARIO_H

#include <string>
#include <utility>
#include <vector>

namespace sil {

struct ScalarProfile {
    /* type: "constant" (value), "ramp" (start->end over
     * [start_s,end_s]), "step" (value, then step_value at step_time_s). */
    std::string type = "constant";
    float value = 0.0f;
    float start = 0.0f;
    float end = 0.0f;
    float start_s = 0.0f;
    float end_s = 0.0f;
    float step_time_s = 0.0f;
    float step_value = 0.0f;

    float at(float t_s) const;
};

struct Scenario {
    /* motor */
    float rs_ohm = 0.05f;
    float ld_h = 1.0e-4f;
    float lq_h = 1.0e-4f;
    float flux_wb = 0.008f;
    int   pole_pairs = 7;
    float inertia_kg_m2 = 1.0e-3f;
    float friction_nm_per_rad_s = 1.0e-3f;
    float vdc_v = 48.0f;
    /* Machine selection mirrors HostSim's motor.machine: "pmsm" (default) or
     * "induction"; the rr/lm/lls/llr fields only apply to induction. */
    std::string machine = "pmsm";
    float rr_ohm = 0.3f;
    float lm_h = 0.025f;
    float lls_h = 0.002f;
    float llr_h = 0.002f;

    /* simulation */
    float duration_s = 2.0f;
    float app_loop_hz = 1000.0f;
    float trace_decim_us = 500.0f;        /* trace row period [us]         */
    float pwm_switching_hz = 0.0f;        /* 0: leave firmware default     */
    std::string trace_csv = "sil_trace.csv";
    std::string fram_image;               /* optional FRAM backing file    */

    /* throttle profiles (normalized [0..1]) */
    ScalarProfile throttle_a;
    ScalarProfile throttle_b;
    bool throttle_b_set = false;          /* false: mirror channel A       */

    /* control engagement after boot */
    bool  control_start = true;
    float control_start_time_s = 1.6f;    /* absolute sim time             */
    float iq_a = 8.0f;
    float id_a = 0.0f;

    /* firmware config KV seeds applied post-boot (config set/save). */
    std::vector<std::pair<std::string, float>> firmware_config;

    /* Scheduled firmware shell commands ("commands": {"<time_s>": "<line>"}):
     * each line is run through CommandManager::processLine exactly like a
     * typed shell command (e.g. "maxcfg_uv 20.0", "foc start 8 0"). */
    std::vector<std::pair<float, std::string>> commands;

    /* Fault injection ("faults" block).  Every fault is a time window
     * [time_s, time_s + duration_s); duration_s <= 0 latches to the end of
     * the run.  A negative time_s (the default) disables the fault.  The
     * injection enters through the modeled sensor/actuator surface, never
     * through the firmware, so the fault response (FaultManager source + reason)
     * is produced by the firmware itself:
     *
     *   vdc_glitch_*     DC-link bus sag seen by the MAX22530 sense channel
     *                    (and the plant).  Arms the firmware UV/OV comparator
     *                    via shell ("maxcfg_uv <v>") to trip Max22530Uv/Ov.
     *   oc_inject_*      Phase-current spike added at the ADC counts level
     *                    (sil_phase_current_adc); 3 consecutive over-threshold
     *                    injected samples raise PhaseOvercurrent (software OC,
     *                    default threshold 500 A, "ocset" to change).
     *   encoder_freeze_* Encoder sample stream stalls (no new DMA samples);
     *                    trips firmware staleness checks (ENCODER_STALE_MS in
     *                    FocControlManager — legacy `foc start` path).
     *   encoder_loss_*   Sin/cos outputs collapse to the bias mid (sensor
     *                    excitation loss); trips EncoderAmplitude in
     *                    EncoderADC::diagnose (Warning severity).
     *   temp_spike_*     Drives one temperature channel (0..2 board, 3 motor)
     *                    to temp_c, through the sensor curve + divider model;
     *                    trips Overtemperature* after the firmware's sustain
     *                    window (500 ms). */
    float vdc_glitch_time_s = -1.0f;
    float vdc_glitch_duration_s = 0.0f;
    float vdc_glitch_v = 0.0f;

    float oc_inject_time_s = -1.0f;
    float oc_inject_duration_s = 0.01f;
    int   oc_inject_phase = 0;          /* 0 = U, 1 = V, 2 = W */
    float oc_inject_a = 600.0f;

    float encoder_freeze_time_s = -1.0f;
    float encoder_freeze_duration_s = 0.0f;

    float encoder_loss_time_s = -1.0f;
    float encoder_loss_duration_s = 0.1f;

    float temp_spike_time_s = -1.0f;
    float temp_spike_duration_s = 0.0f;
    int   temp_spike_channel = 3;       /* 0..2 = board, 3 = motor */
    float temp_spike_c = 200.0f;
};

bool LoadScenario(const char* path, Scenario& out, std::string& error);

} // namespace sil

#endif
