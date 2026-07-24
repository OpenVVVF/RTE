/* Wrap the mechanical encoder angle to [0, 2*pi). */
const float two_pi = 6.28318530718f;
float theta = fmodf(theta_mech, two_pi);
if (theta < 0.0f) theta += two_pi;

/* Electrical angle = offset + sign * encoder_angle * pole_pairs.
 * Matches the base-image FocController convention. */
float elec = offset_rad + encoder_sign * theta * pole_pairs;

/* Wrap the electrical angle to [0, 2*pi). */
elec = fmodf(elec, two_pi);
if (elec < 0.0f) elec += two_pi;

theta_elec = elec;
