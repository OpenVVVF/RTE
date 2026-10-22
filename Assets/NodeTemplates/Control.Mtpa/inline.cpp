/* Maximum Torque Per Ampere (MTPA) current-reference generator.
 * CurrentRef is per-unit [-1..1] and scaled by CurrentMax to obtain the
 * actual current magnitude. For SPM motors (Ld == Lq) the result is
 * Id=0, Iq=CurrentRef*CurrentMax. For IPMSM (Lq > Ld) the current angle
 * is computed from the analytical MTPA condition.
 *
 * Torque equation: T = (3/2)*pp*(Lambda*Iq + (Ld-Lq)*Id*Iq)
 * MTPA condition:  Lambda*cos(beta) + (Ld-Lq)*Is*cos(2*beta) = 0
 *   (beta = current angle from the +d axis: Id = Is*cos(beta), Iq = Is*sin(beta))
 * Solving for cos(beta) with delta = Lq - Ld:
 *   cos(beta) = (Lambda - sqrt(Lambda^2 + 8*delta^2*Is^2)) / (4*delta*Is)
 * The other quadratic root is the anti-MTPA (torque-minimizing) branch; it also
 * diverges as Is -> 0, while the physical root tends to 0 there
 * (cos(beta) ~ -delta*Is/Lambda). For delta > 0 (IPM, Lq > Ld) the physical
 * root is negative: Id < 0 so the reluctance term adds torque for either
 * command sign. For delta < 0 (Ld > Lq) it is positive, Id > 0.
 */
const float i_cmd = CurrentRef * CurrentMax.in(au::amperes);
const float i_abs = fabsf(i_cmd);
float id_ref = 0.0f;

if (i_abs > 1e-6f) {
    const float delta = Lq - Ld;
    if (fabsf(delta) > 1e-9f) {
        const float radical = sqrtf(Lambda * Lambda + 8.0f * delta * delta * i_abs * i_abs);
        float cos_beta = (Lambda - radical) / (4.0f * delta * i_abs);
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
