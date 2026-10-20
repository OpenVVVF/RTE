/* Duty + vdc -> phase-to-neutral-mid voltages with IGBT knee + deadtime +
 * Rce drop (exactly twin/plant/plant.py convention):
 *   leg switching node -> phase-to-mid: (d - d_avg)/50 * vdc
 *   error sign(i) * (v_ce0 + TdOverhead*vdc + r_ce*|i|) subtracted per phase.
 */

double d_avg = (Du + Dv + Dw) / 3.0;
double vu_raw = (Du - d_avg) / 50.0 * Vdc;
double vv_raw = (Dv - d_avg) / 50.0 * Vdc;
double vw_raw = (Dw - d_avg) / 50.0 * Vdc;

auto phase = [&](double v_cmd, double i_p) -> double {
    if (fabs(i_p) < 1e-3) return v_cmd;
    double s_i = i_p > 0.0 ? 1.0 : -1.0;
    return v_cmd - s_i * (Vce0_V + TdOverhead * Vdc + Rce_Ohm * fabs(i_p));
};

V_U = phase(vu_raw, I_U);
V_V = phase(vv_raw, I_V);
V_W = phase(vw_raw, I_W);
