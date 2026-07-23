const float cos_theta = cosf(theta);
const float sin_theta = sinf(theta);
i_dq.d = i_alpha_beta.alpha * cos_theta + i_alpha_beta.beta * sin_theta;
i_dq.q = -i_alpha_beta.alpha * sin_theta + i_alpha_beta.beta * cos_theta;
