/* Async carrier schedule: carrier = lerp(fsw_lo, fsw_hi) across [fe_lo, fe_hi],
 * clamped.  The platform call deadbands small changes, so this can run every
 * app_loop step without churning the timer. */
const float fe = platform_get_elec_freq_hz();
float x = (fe - fe_lo) / (fe_hi - fe_lo);
if (x < 0.0f) x = 0.0f;
if (x > 1.0f) x = 1.0f;
platform_pwm_set_carrier_hz(fsw_lo + x * (fsw_hi - fsw_lo));
