const float cos_theta = cosf(theta);
const float sin_theta = sinf(theta);
v_alpha_beta.alpha = v_dq.d * cos_theta - v_dq.q * sin_theta;
v_alpha_beta.beta = v_dq.d * sin_theta + v_dq.q * cos_theta;
