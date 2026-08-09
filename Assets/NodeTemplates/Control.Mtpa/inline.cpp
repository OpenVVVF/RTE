/* Maximum Torque Per Ampere (MTPA) current-reference generator.
 * CurrentRef is per-unit [-1..1] and scaled by CurrentMax to obtain the
 * actual current magnitude. For SPM motors (Ld == Lq) the result is
 * Id=0, Iq=CurrentRef*CurrentMax. For IPMSM (Lq > Ld) the current angle
 * is computed from the analytical MTPA condition.
 *
 * Torque equation: T = (3/2)*pp*(Lambda*Iq + (Ld-Lq)*Id*Iq)
 * MTPA condition:  Lambda*cos(beta) + (Ld-Lq)*Is*cos(2*beta) = 0
 * Solving for cos(beta) with delta = Lq - Ld:
 *   cos(beta) = (sqrt(Lambda^2 + 8*delta^2*Is^2) - Lambda) / (4*delta*Is)
 */
const float i_cmd = CurrentRef * CurrentMax.in(au::amperes);
const float i_abs = fabsf(i_cmd);
float id_ref = 0.0f;

if (i_abs > 1e-6f) {
    const float delta = Lq - Ld;
    if (fabsf(delta) > 1e-9f) {
        const float radical = sqrtf(Lambda * Lambda + 8.0f * delta * delta * i_abs * i_abs);
        float cos_beta = (radical - Lambda) / (4.0f * delta * i_abs);
        if (cos_beta > 1.0f) cos_beta = 1.0f;
        if (cos_beta < -1.0f) cos_beta = -1.0f;
        id_ref = i_abs * cos_beta;
    }
}

const float ratio = (i_abs > 1e-6f) ? (id_ref / i_abs) : 0.0f;
const float sin_beta = sqrtf(1.0f - ratio * ratio);
const float iq_ref = i_cmd * sin_beta;

I_D = rte::Amperes(id_ref);
I_Q = rte::Amperes(iq_ref);
