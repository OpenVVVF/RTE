/* Finite-control-set model-predictive dq current control (FCS-MPC / MPCC).
 *
 * Model (PMSM dq, forward-Euler over the control period Dt):
 *   id1 = Id + Dt * (vd - Rs*Id + we*Lq*Iq)          / Ld
 *   iq1 = Iq + Dt * (vq - Rs*Iq - we*(Ld*Id + Lambda))/ Lq
 *
 * Candidate set: the 8 two-level three-phase switch states s = (Sa,Sb,Sc)
 * in {0,1}^3. Leg voltages Lk = Sk*Vdc; machine phase-to-mid voltages
 * vk = Lk - (La+Lb+Lc)/3; alpha/beta per the PmsmDq/Park conventions
 * (valpha = (2/3)(va - (vb+vc)/2), vbeta = (vb - vc)/sqrt(3)), rotated
 * into dq with Theta (same frame handed to Transforms.Park).
 *
 * Cost: J = (IdRef-id1)^2 + (IqRef-iq1)^2 + SwitchPenalty*(legs changed).
 * The winning vector is applied DIRECTLY as leg duties (percent 0/100):
 * no SVPWM / modulator stage anywhere in the path - this is the defining
 * MPCC structure ("bus-quantized" voltage set evaluated per control
 * period), and the PI family stays unused.
 *
 * Assumptions: ideal one-period delay-free application of the chosen
 * vector (the 1-period computation delay of a strict FCS-MPC
 * implementation is not modeled - standard bench simplification);
 * back-EMF and cross-coupling terms use the CURRENT measurement
 * (constant over the horizon); knee/deadtime distortion lives in the
 * plant power stage and is seen only through the measured currents.
 */
const float dt = (Dt > 0.0f) ? Dt : 0.0004f;
const float vdc = platform_get_dc_link_voltage();
const float we = RpmElec * (2.0f * 3.14159265358979323846f / 60.0f);

const float ct = cosf(Theta);
const float st = sinf(Theta);

const float sq3 = 1.7320508075688772f;
float best_cost = 1.0e30f;
float best_sa = 0.0f, best_sb = 0.0f, best_sc = 0.0f;
float best_vd = 0.0f, best_vq = 0.0f;

for (int code = 0; code < 8; ++code) {
    const float sa = (code & 4) ? 1.0f : 0.0f;
    const float sb = (code & 2) ? 1.0f : 0.0f;
    const float sc = (code & 1) ? 1.0f : 0.0f;

    /* leg voltages, then common-mode removal (isolated star point) */
    const float la = sa * vdc, lb = sb * vdc, lc = sc * vdc;
    const float v_mid = (la + lb + lc) / 3.0f;
    const float va = la - v_mid;
    const float vb = lb - v_mid;
    const float vc = lc - v_mid;

    const float valpha = (2.0f / 3.0f) * (va - 0.5f * (vb + vc));
    const float vbeta  = (vb - vc) / sq3;
    const float vd = ct * valpha + st * vbeta;
    const float vq = -st * valpha + ct * vbeta;

    const float id1 = Id + dt * (vd - Rs * Id + we * Lq * Iq) / Ld;
    const float iq1 = Iq + dt * (vq - Rs * Iq - we * (Ld * Id + Lambda)) / Lq;

    float cost = (IdRef - id1) * (IdRef - id1) + (IqRef - iq1) * (IqRef - iq1);
    const float legs = fabsf(sa - PrevSa) + fabsf(sb - PrevSb) + fabsf(sc - PrevSc);
    cost += SwitchPenalty * legs;

    if (cost < best_cost) {
        best_cost = cost;
        best_sa = sa; best_sb = sb; best_sc = sc;
        best_vd = vd; best_vq = vq;
    }
}

PrevSa = best_sa;
PrevSb = best_sb;
PrevSc = best_sc;

Duty_A = best_sa * 100.0f;
Duty_B = best_sb * 100.0f;
Duty_C = best_sc * 100.0f;
V_D = best_vd;
V_Q = best_vq;
SwitchCode = best_sa * 4.0f + best_sb * 2.0f + best_sc;
