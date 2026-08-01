/* Six-step modulator (railway ladder TOP mode), PWM'd 120 deg trapezoid.
 *
 * WARNING: six-step belongs at the voltage ceiling only.  Bench-verified
 * (twice): at 20 Hz fundamental on a 10 mohm / low-L motor it explodes to
 * a 250 A limit cycle within milliseconds - full-rail plateaus with tiny
 * back-EMF let phase current free-run between PI corrections.  Do not
 * enable this mode until the overmodulation stage exists and the bench can
 * reach frequencies where sectors are short and back-EMF dominates.
 *
 * Synchronous modulation rule: the pattern is locked to the ROTOR angle
 * (Theta_E from the encoder, rock stable).  The current loop enters only as
 *   - magnitude  m = |V| / (Vdc/sqrt(3)), and
 *   - a smooth phase offset delta = atan2(V_Q, V_D) computed in the dq
 *     frame where commands are quiet DC quantities.
 *
 * Gain match to SVPWM: a 120 deg trapezoid's fundamental is (2*sqrt(3)/pi)
 * times its leg amplitude, so the duty scale is (pi/3) * m.  At m = 1 the
 * duties rail and the pattern naturally becomes full square wave.
 *
 * Contract: ModeId-gated (runs only when ModeId == ModeFrom || ModeId ==
 * ModeTo), holds 50% neutral otherwise; vdc < 1 V -> 50%. */

const float vdc = V_Dc.in(au::volts);
const bool running = (ModeId == ModeFrom) || (ModeId == ModeTo);

if (!running || !(vdc > 1.0f)) {
    Duty_A = 50.0f;
    Duty_B = 50.0f;
    Duty_C = 50.0f;
} else {
    const float vd = V_D.in(au::volts);
    const float vq = V_Q.in(au::volts);

    /* Rotor-locked pattern angle: theta_e + smooth dq phase offset. */
    const float ang = Theta_E + atan2f(vq, vd);

    /* Commanded magnitude relative to the SVPWM linear max (vdc/sqrt(3)),
     * scaled by pi/3 so the trapezoid fundamental equals the commanded
     * fundamental at the ladder boundary. */
    float m = sqrtf(vd * vd + vq * vq) / (vdc * 0.57735026919f);
    if (m > 1.0f) m = 1.0f;
    if (m < 0.0f) m = 0.0f;
    const float s = 1.0471975512f * m;  /* pi/3 * m */

    /* 120-degree trapezoidal references in [-1, 1]: +1 plateau for 120 deg,
     * linear transition over the 60-degree sector boundaries. */
    auto trap = [](float x) -> float {
        const float two_pi = 6.28318530718f;
        x = fmodf(x, two_pi);
        if (x < 0.0f) x += two_pi;
        const float deg60 = 1.04719755120f;
        if (x < deg60) return 1.0f;
        if (x < 2.0f * deg60) return 1.0f - 2.0f * (x - deg60) / deg60;
        if (x < 4.0f * deg60) return -1.0f;
        if (x < 5.0f * deg60) return -1.0f + 2.0f * (x - 4.0f * deg60) / deg60;
        return 1.0f;
    };

    auto duty = [](float d) -> float {
        if (d < 0.0f) return 0.0f;
        if (d > 100.0f) return 100.0f;
        return d;
    };

    Duty_A = duty(50.0f + 50.0f * s * trap(ang));
    Duty_B = duty(50.0f + 50.0f * s * trap(ang - 2.09439510239f));
    Duty_C = duty(50.0f + 50.0f * s * trap(ang + 2.09439510239f));
}
