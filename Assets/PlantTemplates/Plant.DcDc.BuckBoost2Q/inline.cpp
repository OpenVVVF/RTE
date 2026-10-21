/* Two-quadrant buck/boost cell (average model, mirrors the python
 * reference): effective leg voltage = duty * V_High with the knee-class
 * distortion, inductor integrates toward the low side minus winding Rs.
 * Outputs publish from the decaying inductor state even while gates are
 * open. `DtPlant` + ports + parameters are the only runtime symbols. */

double duty = fmin(100.0, fmax(0.0, (double)Duty)) / 100.0;

if (OpenGates >= 0.5) {
    double tau_open = 2.0e-3;
    I_L -= I_L * DtPlant / tau_open;
} else {
    double v_sw = duty * (double)V_High;
    double i = I_L;
    if (fabs(i) >= 1e-3) {
        double s_i = (i > 0.0) ? 1.0 : -1.0;
        v_sw -= s_i * ((double)Vce0_V + (double)TdOverhead * (double)V_High
                       + (double)Rce_Ohm * fabs(i));
    }
    I_L += (v_sw - (double)V_Low - (double)Rs_Winding * i) / (double)L_H * DtPlant;
    I_L = fmin(2000.0, fmax(-2000.0, I_L));
}

I_High = duty * I_L;
P_High = (double)V_High * I_High;
P_Draw = P_High;
I_Low = I_L;
P_Low = (double)V_Low * I_Low;
P_DrawLow = -P_Low;
