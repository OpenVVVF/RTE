const float error = (Setpoint - Measurement).in(au::amperes);
Integral += error * Dt;

float raw_output = Kp * error + Ki * Integral;

/* Dynamic voltage limit derived from DC-link voltage, matching the base
 * image's VectorPIController convention: max = Vdc * 0.5 * MaxModulation. */
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
    Integral -= excess * Dt * AwGain / (Kp * Ki);
}

Output = rte::Volts(limited_output);
