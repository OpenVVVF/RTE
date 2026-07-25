const float error = Setpoint - Measurement;
integral += error;
float raw_output = Kp * error + Ki * integral;
if (raw_output > OutputMax) raw_output = OutputMax;
if (raw_output < OutputMin) raw_output = OutputMin;
Output = raw_output;
