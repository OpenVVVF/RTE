/* Duty + vdc -> phase-to-neutral-mid voltages with IGBT knee + deadtime +
 * Rce drop (exactly twin/plant/plant.py convention):
 *   leg switching node -> phase-to-mid: (d - d_avg)/50 * vdc
 *   error sign(i) * (v_ce0 + TdOverhead*vdc + r_ce*|i|) subtracted per phase.
 */

double d_avg = (Du + Dv + Dw) / 3.0;
double vu_raw = (Du - d_avg) / 50.0 * Vdc;
double vv_raw = (Dv - d_avg) / 50.0 * Vdc;
double vw_raw = (Dw - d_avg) / 50.0 * Vdc;

/* de-energized stage: legs float, all voltage views go to zero (the
 * twin's convention on gate kill) */
if (OpenGates >= 0.5) {
    vu_raw = vv_raw = vw_raw = 0.0;
}

/* pre-distortion command volts: the phase-ADC-visible view (the sensor
 * never sees the knee/deadtime/Rce error applied below) */
V_U_Raw = vu_raw;
V_V_Raw = vv_raw;
V_W_Raw = vw_raw;

auto phase = [&](double v_cmd, double i_p) -> double {
    if (fabs(i_p) < 1e-3) return v_cmd;
    double s_i = i_p > 0.0 ? 1.0 : -1.0;
    return v_cmd - s_i * (Vce0_V + TdOverhead * Vdc + Rce_Ohm * fabs(i_p));
};

if (OpenGates >= 0.5) {
    V_U = V_V = V_W = 0.0;
} else {
    V_U = phase(vu_raw, I_U);
    V_V = phase(vv_raw, I_V);
    V_W = phase(vw_raw, I_W);
}
