Angle += RateRadPerS * Dt;
if (Angle >= 6.28318530718f) Angle -= 6.28318530718f;
else if (Angle < 0.0f) Angle += 6.28318530718f;  /* negative rates wrap back in */
ThetaElec = Angle;
