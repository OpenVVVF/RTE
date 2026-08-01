/* Policy only: watch the electrical frequency (ramp command, FOC estimate,
 * or the pattern's own) and request handoffs.  The switching itself
 * (phase-lock, level match, timer handoff) lives in the base image behind
 * platform_modulation_to_pattern/to_ramp. */
const float fe = platform_get_elec_freq_hz();
const uint8_t mod_mode = platform_modulation_mode();

if (mod_mode == 0U && fe >= enter_hz) {
    (void)platform_modulation_to_pattern((uint32_t)(pulses + 0.5f), duty);
} else if (mod_mode == 1U && fe < exit_hz) {
    (void)platform_modulation_to_ramp();
}
