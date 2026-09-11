/* Guard against a missing/invalid DC-link measurement (precharge, sensor
 * glitch): mirror Gen6 pwm.cpp PWM_SetVoltageVector and drive the midpoint
 * duty.  The !(x > 1) form also rejects NaN/Inf — dividing by a ~0 V link
 * would otherwise emit inf/NaN duties that slip past the percent clamps
 * below (NaN fails x<0/x>100 tests) and permanently poison the downstream
 * PWM/plant state.  Structured as if/else (not an early return) so the
 * rest of the domain step still runs: PwmOut must see the 50% duties. */
const float vdc = V_Dc.in(au::volts);
if (!(vdc > 1.0f)) {
    Duty_A = 50.0f;
    Duty_B = 50.0f;
    Duty_C = 50.0f;
} else {
    /* Clamp the alpha/beta voltage vector to the six-step boundary.
     * The maximum line-to-neutral voltage magnitude for linear modulation is
     * Vdc / sqrt(3); overmodulation is allowed up to 2*Vdc/3. */
    const float sqrt3 = 1.7320508075688772f;
    const float v_max_linear = vdc * 2.0f / 3.0f;
    float valpha = V_Alpha.in(au::volts);
    float vbeta  = V_Beta.in(au::volts);
    const float v_albe_sq = valpha * valpha + vbeta * vbeta;
    if (v_albe_sq > v_max_linear * v_max_linear && v_albe_sq > 1e-12f) {
        const float scale = v_max_linear / sqrtf(v_albe_sq);
        valpha *= scale;
        vbeta  *= scale;
    }

    /* Inverse Clarke: alpha/beta -> A/B/C. */
    const float v_a = valpha / vdc;
    const float v_b = (-0.5f * valpha + 0.86602540378f * vbeta) / vdc;
    const float v_c = (-0.5f * valpha - 0.86602540378f * vbeta) / vdc;

    float v_min = v_a;
    if (v_b < v_min) v_min = v_b;
    if (v_c < v_min) v_min = v_c;

    float v_max = v_a;
    if (v_b > v_max) v_max = v_b;
    if (v_c > v_max) v_max = v_c;

    const float v_offset = 0.5f * (v_min + v_max);

    /* Convert to percent duty and clamp.  Linear SVM stays roughly in
     * [21%, 79%]; clamping to [0,100] only catches numerical edge cases. */
    float duty_a_pct = 50.0f + 50.0f * (v_a - v_offset);
    float duty_b_pct = 50.0f + 50.0f * (v_b - v_offset);
    float duty_c_pct = 50.0f + 50.0f * (v_c - v_offset);

    if (duty_a_pct < 0.0f) duty_a_pct = 0.0f; else if (duty_a_pct > 100.0f) duty_a_pct = 100.0f;
    if (duty_b_pct < 0.0f) duty_b_pct = 0.0f; else if (duty_b_pct > 100.0f) duty_b_pct = 100.0f;
    if (duty_c_pct < 0.0f) duty_c_pct = 0.0f; else if (duty_c_pct > 100.0f) duty_c_pct = 100.0f;

    Duty_A = duty_a_pct;
    Duty_B = duty_b_pct;
    Duty_C = duty_c_pct;
}
