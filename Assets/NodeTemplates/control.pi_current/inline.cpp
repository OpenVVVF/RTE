const float error = (setpoint - measurement).in(au::amperes);
integral += error;
float raw_output = kp * error + ki * integral;
if (raw_output > output_max) raw_output = output_max;
if (raw_output < output_min) raw_output = output_min;
output = rte::Volts(raw_output);
