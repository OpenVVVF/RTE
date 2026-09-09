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
};

bool LoadScenario(const char* path, Scenario& out, std::string& error);

} // namespace sil

#endif
