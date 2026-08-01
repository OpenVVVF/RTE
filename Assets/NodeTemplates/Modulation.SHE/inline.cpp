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
    /* Effective electrical direction.  Dir is the raw RPM sign from the
     * encoder driver, which does NOT include the configured encoder sign
     * (Motor.Encoder.SinCos.Sign) that ElecAngle applies.  On hardware with
     * Sign=-1 the raw RPM is negative for forward rotation, which would
     * otherwise leave pattern mode permanently disarmed while the sequencer
     * enters this mode and the duty path goes neutral - the bench dumped
     * ~900 A of back-EMF current through the legs into the bus that way. */
    const float omega_e = 6.28318530718f * F_Elec * Dir * EncSign;

    platform_pattern_set_command(m, delta, Theta_E, omega_e, (int)NPulses);

    if (Armed < 0.5f && omega_e > 0.0f) {
        Armed = platform_pattern_enable() ? 1.0f : 0.0f;
    }
    ArmOk = (omega_e > 0.0f) ? 1.0f : 0.0f;
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
