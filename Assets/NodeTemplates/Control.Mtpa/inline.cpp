/* Maximum Torque Per Ampere (MTPA) current-reference generator.
 * CurrentRef is per-unit [-1..1] and scaled by CurrentMax to obtain the
 * actual current magnitude. For SPM motors (Ld == Lq) the result is
 * Id=0, Iq=CurrentRef*CurrentMax. For IPMSM (Lq > Ld) the current angle
 * is computed from the analytical MTPA condition.
 *
 * Torque equation: T = (3/2)*pp*(Lambda*Iq + (Ld-Lq)*Id*Iq)
 * The current angle beta is measured from the NEGATIVE d axis, so
 *   Id = -Is*cos(beta),  Iq = Is*sin(beta).
 * MTPA condition:  Lambda*cos(beta) - (Ld-Lq)*Is*cos(2*beta) = 0
 * Solving the resulting quadratic in Is (delta = Lq - Ld) gives the
 * closed-form d-axis reference:
 *   id = (Lambda - sqrt(Lambda^2 + 8*delta^2*Is^2)) / (4*delta)
 * which is negative for IPMSM (delta > 0) and collapses to 0 for SPM.
 */
const float i_cmd = CurrentRef * CurrentMax.in(au::amperes);
const float i_abs = fabsf(i_cmd);
float id_ref = 0.0f;

if (i_abs > 1e-6f) {
    const float delta = Lq - Ld;
    if (fabsf(delta) > 1e-9f) {
        const float radical = sqrtf(Lambda * Lambda + 8.0f * delta * delta * i_abs * i_abs);
        id_ref = (Lambda - radical) / (4.0f * delta);
        /* Keep the reference inside the current circle for degenerate
         * parameter sets (e.g. Ld > Lq at very high current). */
        if (id_ref > i_abs) id_ref = i_abs;
        if (id_ref < -i_abs) id_ref = -i_abs;
    }
}

const float ratio = (i_abs > 1e-6f) ? (id_ref / i_abs) : 0.0f;
const float sin_beta = sqrtf(1.0f - ratio * ratio);
const float iq_ref = i_cmd * sin_beta;

I_D = rte::Amperes(id_ref);
I_Q = rte::Amperes(iq_ref);
