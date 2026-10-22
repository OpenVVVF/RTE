/* Five-leg (10-switch) averaged power stage: exactly the
 * Plant.Inverter.Knee3ph per-leg law extended to five phases A..E:
 *   v_k_raw = (d_k - d_avg)/50 * vdc   (leg node -> phase-to-mid)
 *   v_k     = v_k_raw - sign(i_k) * (Vce0_V + TdOverhead*vdc + Rce_Ohm*|i_k|)
 * d_avg is the mean of ALL five duties, which subtracts the common mode
 * each substep (5-phase open-star machines take no common-mode current;
 * the distortion residual below is the only sum-voltage leakage).
 *
 * Provenance / limits of the model (state, don't fake):
 *  - knee/deadtime/Rce distortion is the 3-phase twin-calibrated law
 *    applied per leg; there is NO recorded 5-phase distortion evidence.
 *  - same-leg interlock/non-ideal propagation beyond this law is not
 *    modeled (same standing simplification as Knee3ph).
 */

double d_avg = (Da + Db + Dc + Dd + De) / 5.0;
double va_raw = (Da - d_avg) / 50.0 * Vdc;
double vb_raw = (Db - d_avg) / 50.0 * Vdc;
double vc_raw = (Dc - d_avg) / 50.0 * Vdc;
double vd_raw = (Dd - d_avg) / 50.0 * Vdc;
double ve_raw = (De - d_avg) / 50.0 * Vdc;

/* de-energized stage: legs float, all voltage views go to zero (the
 * twin's convention on gate kill) */
if (OpenGates >= 0.5) {
    va_raw = vb_raw = vc_raw = vd_raw = ve_raw = 0.0;
}

/* pre-distortion command volts: the phase-ADC-visible view */
V_A_Raw = va_raw;
V_B_Raw = vb_raw;
V_C_Raw = vc_raw;
V_D_Raw = vd_raw;
V_E_Raw = ve_raw;

double distort_A = (fabs(I_A) < 1e-3) ? 0.0 : ((I_A > 0.0) ? 1.0 : -1.0) * (Vce0_V + TdOverhead * Vdc + Rce_Ohm * fabs(I_A));
double distort_B = (fabs(I_B) < 1e-3) ? 0.0 : ((I_B > 0.0) ? 1.0 : -1.0) * (Vce0_V + TdOverhead * Vdc + Rce_Ohm * fabs(I_B));
double distort_C = (fabs(I_C) < 1e-3) ? 0.0 : ((I_C > 0.0) ? 1.0 : -1.0) * (Vce0_V + TdOverhead * Vdc + Rce_Ohm * fabs(I_C));
double distort_D = (fabs(I_D) < 1e-3) ? 0.0 : ((I_D > 0.0) ? 1.0 : -1.0) * (Vce0_V + TdOverhead * Vdc + Rce_Ohm * fabs(I_D));
double distort_E = (fabs(I_E) < 1e-3) ? 0.0 : ((I_E > 0.0) ? 1.0 : -1.0) * (Vce0_V + TdOverhead * Vdc + Rce_Ohm * fabs(I_E));

if (OpenGates >= 0.5) {
    V_A = V_B = V_C = V_D = V_E = 0.0;
} else {
    V_A = va_raw - distort_A;
    V_B = vb_raw - distort_B;
    V_C = vc_raw - distort_C;
    V_D = vd_raw - distort_D;
    V_E = ve_raw - distort_E;
}
