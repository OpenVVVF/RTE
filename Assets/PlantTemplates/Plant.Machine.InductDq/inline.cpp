/* Induction machine in the STATIONARY frame (wf = 0): exactly
 * twin/plant/induction.py. Coupled-L solve per substep:
 *   psi_sd = lsg*isd + Lm*ird ; psi_rd = Lm*isd + lrg*ird  (same in q)
 *   [lsg Lm; Lm lrg][did dir] = [vs - Rs*is + wf*psi_other ...]
 *   w_slip = wf - Omega_M  (frame - machine elec speed)
 *   Te   = 1.5 * pp * (psi_rq*Isd - psi_rd*Isq)
 * With a 0-frame, the dq state IS stationary abc: I_U/alpha = Isd directly.
 */

double lsg = (double)Lls + (double)Lm;
double lrg = (double)Llr + (double)Lm;
double det = lsg * lrg - (double)Lm * (double)Lm;

double valpha = (2.0 / 3.0) * (V_U - 0.5 * (V_V + V_W));
double vbeta  = (2.0 / 3.0) * ((M_SQRT3 / 2.0) * (V_V - V_W));
double vd = valpha;
double vq = vbeta;

double wf = 0.0;                              // stationary frame
double w_slip = wf - (double)Omega_Elec;      // frame - machine elec speed

double psi_sd = lsg * Isd + (double)Lm * Ird;
double psi_sq = lsg * Isq + (double)Lm * Irq;
double psi_rd = (double)Lm * Isd + lrg * Ird;
double psi_rq = (double)Lm * Isq + lrg * Irq;

double bd0 = vd - (double)Rs * Isd + wf * psi_sq;
double bd1 = -(double)Rr * Ird + w_slip * psi_rq;
double bq0 = vq - (double)Rs * Isq - wf * psi_sd;
double bq1 = -(double)Rr * Irq - w_slip * psi_rd;

double did  = (bd0 * lrg - (double)Lm * bd1) / det;
double didr = (lsg * bd1 - (double)Lm * bd0) / det;
double diq  = (bq0 * lrg - (double)Lm * bq1) / det;
double diqr = (lsg * bq1 - (double)Lm * bq0) / det;

if (OpenGates >= 0.5) {
    /* six-switch-open freewheel collapse (twin/plant) */
    double tau_open = 2.0e-3;
    Isd -= Isd * DtPlant / tau_open;
    Isq -= Isq * DtPlant / tau_open;
    Ird -= Ird * DtPlant / tau_open;
    Irq -= Irq * DtPlant / tau_open;
} else {
    Isd += did * DtPlant;
    Ird += didr * DtPlant;
    Isq += diq * DtPlant;
    Irq += diqr * DtPlant;
}

// physical rails
Isd = fmin(2000.0, fmax(-2000.0, Isd));
Isq = fmin(2000.0, fmax(-2000.0, Isq));
Ird = fmin(2000.0, fmax(-2000.0, Ird));
Irq = fmin(2000.0, fmax(-2000.0, Irq));

Torque = 1.5 * Poles * (psi_rd * Isq - psi_rq * Isd);
// air-gap (link-side) power = Te * syn speed: nomenclature plant.py; the
// synchronous speed is a controller-side external (unwired = 0, matching
// the python zero-binding), gated to zero while gates are open
P_Elec = (OpenGates >= 0.5) ? 0.0 : Torque * Omega_Cmd / Poles;

// abc stator currents at the stationary frame: alpha := U directly
I_U = Isd;
I_V = -0.5 * Isd + (M_SQRT3 / 2.0) * Isq;
I_W = -I_U - I_V;
