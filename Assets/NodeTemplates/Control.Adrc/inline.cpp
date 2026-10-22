/* Active Disturbance Rejection Control (first-order plant):
 *   dy/dt = f_total + b0 * u
 * Used here as a PMSM dq-current regulator (drop-in sibling of
 * Control.Ladrc / Control.Pi).
 *
 * What makes this node ADRC (and distinct from Control.Ladrc):
 *   - the linear extended-state observer (ESO) uses the pole-pair gain
 *     family parameterized by (OmegaL, ObsDamp):
 *         beta1 = 2 * zeta * wl, beta2 = wl^2
 *     (zeta = 1 reproduces the Ladrc double-pole family beta = (2*wo, wo^2);
 *     zeta = 1/sqrt(2) is the Butterworth placement), and
 *   - the ESO disturbance estimate is lifted out as outputs:
 *       DistRate  = z2        (A/s  — total disturbance rate),
 *       DistVolts = z2 / b0   (volts — equivalent input the law cancels),
 *     so the disturbance-rejection behavior is measurable in telemetry.
 *
 * ESO update (forward Euler, same discretization family as Control.Ladrc):
 *   e  = z1 - y
 *   z1 += dt * (z2 + b0*u_prev - beta1*e)
 *   z2 += dt * (-beta2 * e)
 * Control (B0-lifted):
 *   u0 = wc * (r - z1)
 *   u  = (u0 - z2) / b0
 *
 * Assumptions: scalar first-order plant model per dq axis (cross-coupling,
 * back-EMF and Rs drop live inside f_total and are rejected, not modeled);
 * one-step applied-voltage delay carried by UPrev.
 */
float dt = Dt;
float b0 = B0;
float wc = OmegaC;
float wl = OmegaL;
float zeta = ObsDamp;
if (!(dt > 0.0f)) dt = 0.0002f;
if (!(b0 > 1.0e-6f) && !(b0 < -1.0e-6f)) b0 = 10000.0f; /* ~1/100µH */
if (!(wc > 0.0f)) wc = 800.0f;
if (!(wl > 0.0f)) wl = 2400.0f;
if (!(zeta > 0.0f)) zeta = 1.0f;

const float y = Measurement;
const float r = Setpoint;

const float beta1 = 2.0f * zeta * wl;
const float beta2 = wl * wl;

float z1 = Z1;
float z2 = Z2;
/* Cold-start: align observer to measurement once. */
if (z1 == 0.0f && z2 == 0.0f && UPrev == 0.0f) {
    z1 = y;
}

const float u_prev = UPrev;
const float e_obs = z1 - y;
z1 += dt * (z2 + b0 * u_prev - beta1 * e_obs);
z2 += dt * (-beta2 * e_obs);
Z1 = z1;
Z2 = z2;

const float u0 = wc * (r - z1);
float u = (u0 - z2) / b0;

/* Same Vdc-aware clamp convention as Control.Pi: allow overmodulation up
 * to the six-step boundary (2*Vdc/3). */
const float vdc = platform_get_dc_link_voltage();
const float dynamic_max = vdc * 2.0f / 3.0f;
float max_limit = (dynamic_max < OutputMax) ? dynamic_max : OutputMax;
float min_limit = (-dynamic_max > OutputMin) ? -dynamic_max : OutputMin;
if (u > max_limit) u = max_limit;
if (u < min_limit) u = min_limit;

UPrev = u;
Output = u;
EstState = z1;
DistRate = z2;
DistVolts = z2 / b0;
