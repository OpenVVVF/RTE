/* Five-phase PMSM dq-Euler integration. Fundamental-plane semantics follow
 * twin/plant/plant.py (Plant.Machine.PmsmDq) exactly; the z1/z2 plane adds
 * the two extra orthogonal axes of the five-axis Clarke projection.
 * Runtime symbols: only parameters/substituted ports and `DtPlant`.
 *
 * Five-phase orthogonal Clarke (amplitude-invariant 2/5 factor, the
 * 5-phase analogue of PmsmDq's 2/3; legs A..E at 72 deg electrical):
 *   valpha = (2/5) sum_k v_k * cos(k*72deg),  vbeta = (2/5) sum_k v_k * sin(k*72deg)
 *   vz1    = (2/5) sum_k v_k * cos(2k*72deg), vz2   = (2/5) sum_k v_k * sin(2k*72deg)
 *   k = 0..4 for legs A, B, C, D, E.
 *   (zs not integrated: open-star machine, no common-mode current path)
 * dq projection onto the rotor frame as in PmsmDq:
 *   vd = cos(th)*valpha + sin(th)*vbeta ; vq = -sin(th)*valpha + cos(th)*vbeta
 *
 * Ordering rules mirrored from PmsmDq: ALL derivatives are computed from
 * the PRE-update state into temporaries first, then assigned; phase
 * currents/torque are rebuilt from the FINAL (id, iq, iz1, iz2, theta)
 * this substep; P_Elec is zero while the gates are open (the collapse
 * returns energy via the machine state decay, not the air-gap term).
 *
 *   did  = (vd  - Rs*Id + Omega*Lq*Iq) / Ld
 *   diq  = (vq  - Rs*Iq - Omega*(Ld*Id + Lambda)) / Lq
 *   diz1 = (vz1 - Rs*Iz1) / Lz        (z-plane has no EMF content for a
 *   diz2 = (vz2 - Rs*Iz2) / Lz         sinusoidal-distributed machine)
 *
 * Torque/power (peak per-phase convention, N/2 with N = 5):
 *   Te = (5/2)*Poles*(Lambda*Iq + (Ld - Lq)*Id*Iq)
 *   P_Elec = Te * Omega / Poles
 * Model simplifications (with provenance): z-plane rotor/mutual coupling
 * neglected; star-point displacement ignored; Lz is a calibration gap.
 * Full list: PROVENANCE.md in this template folder.
 */

double ct = cos((double)ThetaE);
double st = sin((double)ThetaE);

double phi72 = 2.0 * M_PI / 5.0;

/* five-axis Clarke of the winding voltages */
double valpha = 0.4 * (V_A
                     + V_B * cos(1.0 * phi72) + V_C * cos(2.0 * phi72)
                     + V_D * cos(3.0 * phi72) + V_E * cos(4.0 * phi72));
double vbeta  = 0.4 * (V_B * sin(1.0 * phi72) + V_C * sin(2.0 * phi72)
                     + V_D * sin(3.0 * phi72) + V_E * sin(4.0 * phi72));
double vz1 = 0.4 * (V_A
                  + V_B * cos(2.0 * phi72) + V_C * cos(4.0 * phi72)
                  + V_D * cos(6.0 * phi72) + V_E * cos(8.0 * phi72));
double vz2 = 0.4 * (V_B * sin(2.0 * phi72) + V_C * sin(4.0 * phi72)
                  + V_D * sin(6.0 * phi72) + V_E * sin(8.0 * phi72));

double vd = ct * valpha + st * vbeta;
double vq = -st * valpha + ct * vbeta;

if (OpenGates >= 0.5) {
    /* open-circuit freewheel collapse on every current axis (twin
     * 2 ms convention, mirrored from PmsmDq) */
    double tau_open = 2.0e-3;
    Id  -= Id  * DtPlant / tau_open;
    Iq  -= Iq  * DtPlant / tau_open;
    Iz1 -= Iz1 * DtPlant / tau_open;
    Iz2 -= Iz2 * DtPlant / tau_open;
} else {
    double did  = (vd - Rs * Id + Omega_Elec * Lq * Iq) / Ld;
    double diq  = (vq - Rs * Iq - Omega_Elec * (Ld * Id + Lambda)) / Lq;
    double diz1 = (vz1 - Rs * Iz1) / Lz;
    double diz2 = (vz2 - Rs * Iz2) / Lz;
    Id  += did  * DtPlant;
    Iq  += diq  * DtPlant;
    Iz1 += diz1 * DtPlant;
    Iz2 += diz2 * DtPlant;
}
ThetaE = fmod((double)ThetaE + Omega_Elec * DtPlant, 2.0 * M_PI);
if (ThetaE < 0.0) ThetaE += 2.0 * M_PI;

/* physical state rails (PmsmDq catalogue) */
Id  = fmin(2000.0, fmax(-2000.0, Id));
Iq  = fmin(2000.0, fmax(-2000.0, Iq));
Iz1 = fmin(2000.0, fmax(-2000.0, Iz1));
Iz2 = fmin(2000.0, fmax(-2000.0, Iz2));

Torque  = 2.5 * Poles * (Lambda * Iq + (Ld - Lq) * Id * Iq);
P_Elec = (OpenGates >= 0.5) ? 0.0 : Torque * Omega_Elec / Poles;

/* phase currents from the FINAL state this substep (sensor instant):
 * pseudo-inverse of the forward projection */
double ct2 = cos((double)ThetaE);
double st2 = sin((double)ThetaE);
I_A = ct2 * Id - st2 * Iq + Iz1;
I_B = cos((double)ThetaE - 1.0 * phi72) * Id - sin((double)ThetaE - 1.0 * phi72) * Iq
    + cos(2.0 * phi72) * Iz1 + sin(2.0 * phi72) * Iz2;
I_C = cos((double)ThetaE - 2.0 * phi72) * Id - sin((double)ThetaE - 2.0 * phi72) * Iq
    + cos(4.0 * phi72) * Iz1 + sin(4.0 * phi72) * Iz2;
I_D = cos((double)ThetaE - 3.0 * phi72) * Id - sin((double)ThetaE - 3.0 * phi72) * Iq
    + cos(6.0 * phi72) * Iz1 + sin(6.0 * phi72) * Iz2;
I_E = cos((double)ThetaE - 4.0 * phi72) * Id - sin((double)ThetaE - 4.0 * phi72) * Iq
    + cos(8.0 * phi72) * Iz1 + sin(8.0 * phi72) * Iz2;
