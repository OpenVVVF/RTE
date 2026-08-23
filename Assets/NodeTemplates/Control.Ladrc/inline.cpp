/* Linear ADRC (LADRC) for a first-order plant:
 *   dy/dt = f_total + b0 * u
 * Used here as a PMSM dq-current regulator (drop-in for Control.Pi).
 *
 * ESO (Gao bandwidth parameterization):
 *   e  = z1 - y
 *   z1 += dt * (z2 + b0*u - 2*ωo*e)
 *   z2 += dt * (-ωo^2 * e)
 * Control:
 *   u0 = ωc * (r - z1)
 *   u  = (u0 - z2) / b0
 *
 * Independent of FOC PI and FCS-MPCC implementations.
 */
float dt = Dt;
float b0 = B0;
float wc = OmegaC;
float wo = OmegaO;
if (!(dt > 0.0f)) dt = 0.0002f;
if (!(b0 > 1.0e-6f) && !(b0 < -1.0e-6f)) b0 = 10000.0f; /* ~1/100µH */
if (!(wc > 0.0f)) wc = 800.0f;
if (!(wo > 0.0f)) wo = 2400.0f;

const float y = Measurement;
const float r = Setpoint;

const float beta1 = 2.0f * wo;
const float beta2 = wo * wo;

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
