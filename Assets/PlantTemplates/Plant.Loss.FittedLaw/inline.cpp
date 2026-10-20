/* Fitted total-loss law (twin/plant/plant.py v2; calibrated 2026-10-20).
 * `Idle` is zero-bound by default when unconnected = 'live' (losses on).
 * Domain guard: floor at 0 - the polynomial is a data-law over the fitted
 * campaign span and extrapolates negative at induction-idle points; the
 * floor prevents energy-creation artifacts. */
double idle = Idle;   // optional-wired; 0 = live (default), >=0.5 = gates off
double lt = (K1 * fabs(Im) + K2 * Im * Im + K3 * fabs(Om)
             + K4 * fabs(Im) * fabs(Om) + K5 * Vdc * fabs(Im));
if (idle >= 0.5) lt = 0.0;
if (lt < 0.0) lt = 0.0;
P_Loss = lt;
P_Plate = lt * PlateShare;
