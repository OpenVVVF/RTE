const float v_a = v_alpha_beta.beta.in(au::volts) / vdc.in(au::volts);
const float v_b = (-0.5f * v_alpha_beta.beta + 0.86602540378f * v_alpha_beta.alpha).in(au::volts) / vdc.in(au::volts);
const float v_c = (-0.5f * v_alpha_beta.beta - 0.86602540378f * v_alpha_beta.alpha).in(au::volts) / vdc.in(au::volts);

float v_min = v_a;
if (v_b < v_min) v_min = v_b;
if (v_c < v_min) v_min = v_c;

float v_max = v_a;
if (v_b > v_max) v_max = v_b;
if (v_c > v_max) v_max = v_c;

const float v_offset = 0.5f * (v_min + v_max);

duty_abc.a = 0.5f + (v_a - v_offset);
duty_abc.b = 0.5f + (v_b - v_offset);
duty_abc.c = 0.5f + (v_c - v_offset);
