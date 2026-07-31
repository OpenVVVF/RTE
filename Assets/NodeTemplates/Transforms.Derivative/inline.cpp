/* Filtered derivative: Out ~= d(In)/dt, low-passed at Fc [Hz] to keep
 * sampling noise out of the estimate (Fc <= 0 disables filtering).
 * Prev/OutState are persistent state (leave at graph defaults).
 *
 * Unwrap ±2π jumps so differentiating a wrapped angle (θe) does not produce
 * thousands of rad/s spikes at wrap — that was commanding huge MPCC voltage. */
float d = In - Prev;
const float pi = 3.14159265359f;
const float two_pi = 6.28318530718f;
if (d > pi) d -= two_pi;
if (d < -pi) d += two_pi;
const float raw = d / Dt;
Prev = In;
if (Fc > 0.0f) {
    const float alpha = 1.0f / (1.0f + 1.0f / (6.28318530718f * Fc * Dt));
    OutState += alpha * (raw - OutState);
} else {
    OutState = raw;
}
Out = OutState;
