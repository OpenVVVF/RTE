const float error = (setpoint - measurement).in(au::amperes);
integral += error * dt_s;

float raw_output = kp * error + ki * integral;

/* Dynamic voltage limit derived from DC-link voltage, matching the base
 * image's VectorPIController convention: max = Vdc * 0.5 * MaxModulation. */
const float vdc = platform_get_dc_link_voltage();
const float dynamic_max = vdc * 0.5f * 0.9f;
const float max_limit = (dynamic_max < output_max) ? dynamic_max : output_max;
const float min_limit = (-dynamic_max > output_min) ? -dynamic_max : output_min;

float limited_output = raw_output;
if (limited_output > max_limit) limited_output = max_limit;
if (limited_output < min_limit) limited_output = min_limit;

/* Back-calculation anti-windup (matches base-image VectorPIController). */
if (ki > 0.0001f && kp > 0.0001f) {
    const float excess = raw_output - limited_output;
    integral -= excess * dt_s / (kp * ki);
}

output = rte::Volts(limited_output);
