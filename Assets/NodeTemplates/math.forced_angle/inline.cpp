angle += rate_rad_per_s * dt_s;
if (angle >= 6.28318530718f) angle -= 6.28318530718f;
theta_elec = angle;
