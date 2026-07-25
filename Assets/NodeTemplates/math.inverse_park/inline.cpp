const float cos_theta = cosf(Theta);
const float sin_theta = sinf(Theta);
V_Alpha = V_D * cos_theta - V_Q * sin_theta;
V_Beta = V_D * sin_theta + V_Q * cos_theta;
