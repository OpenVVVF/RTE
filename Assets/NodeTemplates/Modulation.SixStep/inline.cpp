/* Six-step modulator (railway ladder top mode), PWM'd 120 deg trapezoid.
 *
 * Contract (see Assets/NodeTemplates/README.md):
 * - Pure per-step law: no frequency math, no mode selection, no enables.
 * - Runs only when gated: ModeId == ModeFrom || ModeId == ModeTo; otherwise
 *   holds 50% neutral.
 * - arm()/continuity: the pattern is driven directly by the commanded
 *   voltage vector (same vector the carrier mode sees), so phase is
 *   inherently locked and magnitude is normalized to the same reference
 *   (Vdc/sqrt(3)) as SVPWM - the handoff needs no duty crossfade.
 * - Safe for milliohm motors: every phase keeps chopping at the carrier;
 *   the average phase voltage follows the commanded magnitude.
 *
 * True n-pulse (SHE) modulators replace the trapezoid with notch tables
 * while keeping this exact contract. */

const float vdc = V_Dc.in(au::volts);
const bool running = (ModeId == ModeFrom) || (ModeId == ModeTo);

if (!running || !(vdc > 1.0f)) {
    Duty_A = 50.0f;
    Duty_B = 50.0f;
    Duty_C = 50.0f;
} else {
    const float valpha = V_Alpha.in(au::volts);
    const float vbeta = V_Beta.in(au::volts);
    const float ang = atan2f(vbeta, valpha);

    /* Commanded magnitude relative to the SVPWM linear max (vdc/sqrt(3));
     * trapezoid plateaus reach the rail at m = 1. */
    float m = sqrtf(valpha * valpha + vbeta * vbeta) / (vdc * 0.57735026919f);
    if (m > 1.0f) m = 1.0f;
    if (m < 0.0f) m = 0.0f;

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

    Duty_A = 50.0f + 50.0f * m * trap(ang);
    Duty_B = 50.0f + 50.0f * m * trap(ang - 2.09439510239f);
    Duty_C = 50.0f + 50.0f * m * trap(ang + 2.09439510239f);
}
