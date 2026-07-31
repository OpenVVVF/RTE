/* Wrap the mechanical encoder angle to [0, 2*pi). */
const float two_pi = 6.28318530718f;
float theta = fmodf(ThetaMech, two_pi);
if (theta < 0.0f) theta += two_pi;

/* Electrical angle = offset (elec deg) + sign * encoder_angle * (Poles / 2).
 * Matches the base-image FocController convention (CyclesRev=1).
 *
 * Guard bad FRAM/config values that freeze θe at 0:
 *  - Poles<=0  → fall back to 10
 *  - |Sign|<0.5 → fall back to +1 (Sign=0 nulls the encoder term).
 *    Do NOT coerce a valid Sign=-1. */
constexpr float DEG_TO_RAD = 0.01745329251f;
float poles = Poles;
if (!(poles > 0.5f)) poles = 10.0f;
float sign = EncoderSign;
if (fabsf(sign) < 0.5f) sign = 1.0f;
else sign = (sign >= 0.0f) ? 1.0f : -1.0f;
float offset_deg = OffsetDeg;
float elec = offset_deg * DEG_TO_RAD + sign * theta * poles * 0.5f;

/* Wrap the electrical angle to [0, 2*pi). */
elec = fmodf(elec, two_pi);
if (elec < 0.0f) elec += two_pi;

ThetaElec = elec;
