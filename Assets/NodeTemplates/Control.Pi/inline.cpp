/* PI with clamping + back-calculation anti-windup.
 * Dimensionless throughout: implicit unit extraction/injection handles any
 * physical-quantity wiring at the binding sites.
 * Sample period follows the live control-ISR rate (tracks the carrier when
 * Modulation.CarrierSet changes it); falls back to the Dt parameter. */
float dt = platform_get_pwm_dt();
if (!(dt > 0.0f)) dt = Dt;

const float error = Setpoint - Measurement;
Integral += error * dt;

float raw_output = Kp * error + Ki * Integral;

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
if (Ki > 0.0001f && Kp > 0.0001f && AwGain > 0.0f) {
    const float excess = raw_output - limited_output;
    Integral -= excess * dt * AwGain / (Kp * Ki);
}

Output = limited_output;
