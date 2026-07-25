const float cos_theta = cosf(theta);
const float sin_theta = sinf(theta);
v_alpha = vd * cos_theta - vq * sin_theta;
v_beta = vd * sin_theta + vq * cos_theta;
