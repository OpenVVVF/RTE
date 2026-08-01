/* SHE pattern modulator (ladder mode 2): thin command layer over the
 * base-image PatternModulator, which owns TIM1 with edges scheduled at
 * exact SHE table angles while this mode is gated on.
 *
 * Contract: runs only when ModeId == ModeFrom || ModeId == ModeTo.  On the
 * gate rising edge it enables pattern mode (the driver arms phase-locked
 * from the current command); on the falling edge it disables (duty path
 * restored at 50%).  Duty outputs are dummy neutral; PwmOut is a no-op
 * while pattern mode is enabled (platform layer gates it). */

const bool running = (ModeId == ModeFrom) || (ModeId == ModeTo);

if (running) {
    const float vd = V_D.in(au::volts);
    const float vq = V_Q.in(au::volts);
    const float vdc = V_Dc.in(au::volts);

    float m = 0.0f;
    if (vdc > 1.0f) {
        m = sqrtf(vd * vd + vq * vq) / (vdc * 0.57735026919f);
        if (m > 1.15f) m = 1.15f;
        if (m < 0.0f) m = 0.0f;
    }
    const float delta = atan2f(vq, vd);
    /* Signed electrical speed: F_Elec is a magnitude, Dir carries sign. */
    const float omega_e = 6.28318530718f * F_Elec * Dir;

    platform_pattern_set_command(m, delta, Theta_E, omega_e, (int)NPulses);

    if (Armed < 0.5f && Dir > 0.0f) {
        Armed = 1.0f;
        platform_pattern_enable();
    }
} else {
    if (Armed > 0.5f) {
        Armed = 0.0f;
        platform_pattern_disable();
    }
}

Duty_A = 50.0f;
Duty_B = 50.0f;
Duty_C = 50.0f;
PatternOn = platform_pattern_is_enabled() ? 1.0f : 0.0f;
