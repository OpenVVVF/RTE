/* Clamp the alpha/beta voltage vector to the six-step boundary.
 * The maximum line-to-neutral voltage magnitude for linear modulation is
 * Vdc / sqrt(3); overmodulation is allowed up to 2*Vdc/3. */
if (V_Dc.in(au::volts) <= 0.001f) {
    Duty_A = 0.0f;
    Duty_B = 0.0f;
    Duty_C = 0.0f;
    return;
}

const float sqrt3 = 1.7320508075688772f;
const float v_max_linear = V_Dc.in(au::volts) * 2.0f / 3.0f;
float valpha = V_Alpha.in(au::volts);
float vbeta  = V_Beta.in(au::volts);
const float v_albe_sq = valpha * valpha + vbeta * vbeta;
if (v_albe_sq > v_max_linear * v_max_linear && v_albe_sq > 1e-12f) {
    const float scale = v_max_linear / sqrtf(v_albe_sq);
    valpha *= scale;
    vbeta  *= scale;
}

/* Inverse Clarke: alpha/beta -> A/B/C. */
const float v_a = valpha / V_Dc.in(au::volts);
const float v_b = (-0.5f * valpha + 0.86602540378f * vbeta) / V_Dc.in(au::volts);
const float v_c = (-0.5f * valpha - 0.86602540378f * vbeta) / V_Dc.in(au::volts);

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
