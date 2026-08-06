/* Adaptive current-sample trigger:
 *   Place the ADC micro-burst trigger at the quietest point in the upcoming
 *   PWM period, computed from the three phase duties.  Falls back to the
 *   legacy bottom trigger when no clean window exists.
 *
 * Connect after the SVPWM node so the duties reflect the upcoming period.
 */
const uint32_t arr = platform_pwm_get_arr();
const uint32_t gap = platform_schedule_adaptive_sample(
    Duty_A, Duty_B, Duty_C, arr);

GapTicks = rte::Dimensionless(static_cast<float>(gap));
