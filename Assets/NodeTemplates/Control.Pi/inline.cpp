/* PI with clamping + back-calculation anti-windup.
 * Dimensionless throughout: implicit unit extraction/injection handles any
 * physical-quantity wiring at the binding sites.
 *
 * dt comes from the live control rate (platform_get_control_dt) so integral
 * gain stays correct when the carrier changes at runtime (CarrierAuto);
 * the Dt parameter is kept for backward compatibility but ignored.
 *
 * Rate-adaptive gain scheduling: gains are tuned for the DtRef update rate
 * (0.0002 s = 5 kHz).  When the live rate is slower, Kp/Ki back off
 * proportionally so loop-delay margin stays constant across the carrier
 * sweep; faster rates never boost above the tuned gains. */
const float dt_s = platform_get_control_dt();
float gscale = (DtRef > 0.0f && dt_s > 0.0f) ? DtRef / dt_s : 1.0f;
if (gscale > 1.0f) gscale = 1.0f;
const float KpEff = Kp * gscale;
const float KiEff = Ki * gscale;

const float error = Setpoint - Measurement;
Integral += error * dt_s;

float raw_output = KpEff * error + KiEff * Integral;

/* Dynamic limit derived from DC-link voltage, matching the base image's
 * VectorPIController convention: max = Vdc/sqrt(3) * 0.95. */
const float vdc = platform_get_dc_link_voltage();
const float dynamic_max = (vdc / 1.7320508075688772f) * 0.95f;
const float max_limit = (dynamic_max < OutputMax) ? dynamic_max : OutputMax;
const float min_limit = (-dynamic_max > OutputMin) ? -dynamic_max : OutputMin;

float limited_output = raw_output;
if (limited_output > max_limit) limited_output = max_limit;
if (limited_output < min_limit) limited_output = min_limit;

/* Back-calculation anti-windup, scaled by AwGain (0 disables; 1.0 matches
 * the base-image VectorPIController). */
if (KiEff > 0.0001f && KpEff > 0.0001f && AwGain > 0.0f) {
    const float excess = raw_output - limited_output;
    Integral -= excess * dt_s * AwGain / (KpEff * KiEff);
}

Output = limited_output;
