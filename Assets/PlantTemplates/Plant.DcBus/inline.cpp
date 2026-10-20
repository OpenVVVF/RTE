/* DC bus integrator (exactly twin/plant/plant.py bench-supply semantics):
 *   i_sup = (v_set - v)/r_s, clamped to [sink, i_limit]; optional CV clamp
 *   branch sinks above cv_clamp_v.
 *   C dv/dt = i_sup - i_inv;  i_inv = P_Draw / max(vdc, 1).
 *   physical rails clamp keeps solver sane at supply-collapse boundaries.
 */

double r_s = fmax((double)SeriesR_mOhm / 1e3, 1e-9);
double i_sup = ((double)VSet_V - Vdc) / r_s;
double lo = (CanSink >= 0.5) ? -SinkLimit_A : 0.0;
double hi = ILimit_A;
if (CvClamp_V > 0.0 && Vdc > CvClamp_V) {
    i_sup = -fmin((Vdc - (double)CvClamp_V) / r_s, (double)CvClampSink_A);
} else {
    i_sup = fmax(lo, fmin(i_sup, hi));
}
double i_inv = P_Draw / fmax((double)Vdc, 1.0);
Vdc += (i_sup - i_inv) / (double)Cap_F * DtPlant;
Vdc = fmin(fmax((double)Vdc, 0.5), 2000.0);

// regen-feed convention: positive = into the link (matches recorded dclink_*;
// twin/plant.py emits -p_dc / -p_dc/max(vdc,1) against the updated bus)
P_Link = -P_Draw;
I_Link = -P_Draw / fmax((double)Vdc, 1.0);
