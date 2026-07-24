float angle_deg = 0.0f;
if (platform_get_encoder_angle(&angle_deg)) {
    theta = angle_deg * 0.01745329251f;  // deg -> rad
}
