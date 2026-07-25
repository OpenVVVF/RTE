/* Wrap the mechanical encoder angle to [0, 2*pi). */
const float two_pi = 6.28318530718f;
float theta = fmodf(ThetaMech, two_pi);
if (theta < 0.0f) theta += two_pi;

/* Electrical angle = offset + sign * encoder_angle * (Poles / 2).
 * Matches the base-image FocController convention. */
float elec = OffsetRad + EncoderSign * theta * Poles * 0.5f;

/* Wrap the electrical angle to [0, 2*pi). */
elec = fmodf(elec, two_pi);
if (elec < 0.0f) elec += two_pi;

ThetaElec = elec;
