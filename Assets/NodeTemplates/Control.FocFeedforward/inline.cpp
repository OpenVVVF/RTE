/* PMSM voltage feed-forward: cross-coupling and back-EMF terms.
 * Electrical speed is supplied in RPM and converted to rad/s internally.
 *
 *   Vd_ff = -ωe * Lq * Iq
 *   Vq_ff =  ωe * Ld * Id + ωe * Lambda
 */
constexpr float RPM_TO_RAD_S = 2.0f * 3.14159265358979323846f / 60.0f;
const float omega_e = RpmElec * RPM_TO_RAD_S;

const float id_a = I_D.in(au::amperes);
const float iq_a = I_Q.in(au::amperes);

const float vd_ff = -(omega_e * Lq * iq_a);
const float vq_ff = (omega_e * Ld * id_a) + (omega_e * Lambda);

V_D = rte::Volts(vd_ff);
V_Q = rte::Volts(vq_ff);
