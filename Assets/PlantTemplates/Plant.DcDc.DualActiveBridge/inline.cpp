/* Dual Active Bridge (average square-wave power law; combinational, mirrors
 * the python reference): P = V1 * V2' * phi * (pi - |phi|) / (2 pi^2 f L).
 * V2' is the LV voltage referred to the HV side (V_Low * Ratio_HL). */

if (OpenGates >= 0.5) {
    P_Transfer = 0.0;
    I_High = 0.0;
    I_Low = 0.0;
    P_DrawHigh = 0.0;
    P_DrawLow = 0.0;
} else {
    double phi = fmin(M_PI, fmax(-M_PI, (double)PhaseShift));
    double v1 = (double)V_High;
    double v2 = (double)V_Low * (double)Ratio_HL;   // LV side seen on HV swing
    if ((double)Lk_H <= 0.0 || (double)Fsw_Hz <= 0.0 || v1 <= 0.0 || v2 <= 0.0) {
        P_Transfer = 0.0;
        I_High = 0.0;
        I_Low = 0.0;
        P_DrawHigh = 0.0;
        P_DrawLow = 0.0;
    } else {
        P_Transfer = v1 * v2 * phi * (M_PI - fabs(phi))
                   / (2.0 * M_PI * M_PI * (double)Fsw_Hz * (double)Lk_H);
        I_High = P_Transfer / v1;
        /* receiver convention: the LV-side current flowing INTO the bridge;
         * while the HV side delivers (phi>0) the LV bus receives, and its
         * outflow is negative */
        I_Low = -P_Transfer / (double)V_Low;
    }
}
P_DrawHigh = P_Transfer;
P_DrawLow = -P_Transfer;
