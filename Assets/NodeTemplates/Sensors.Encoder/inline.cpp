/* Mechanical angle [rad] and speed [rpm] from the base-image encoder driver. */
Theta = platform_get_encoder_angle_latest() * 0.01745329251f;  // deg -> rad
Omega = platform_get_motor_rpm();
