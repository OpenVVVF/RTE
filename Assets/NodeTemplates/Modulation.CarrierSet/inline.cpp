/* Apply-on-change carrier frequency control.  Runs in app_loop so the
 * timer reconfiguration happens in thread context, not inside the PWM ISR. */
float target = TargetHz;
if (target < MinHz) target = MinHz;
if (target > MaxHz) target = MaxHz;

if (fabsf(target - Applied) > 1.0f) {
    if (platform_set_pwm_frequency(target)) {
        Applied = target;
    }
}
AppliedHz = platform_get_pwm_carrier_hz();
