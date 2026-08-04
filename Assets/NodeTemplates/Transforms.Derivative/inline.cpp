/* Filtered derivative: Out ~= d(In)/dt, low-passed at Fc [Hz] to keep
 * sampling noise out of the estimate (Fc <= 0 disables filtering).
 * Prev/OutState are persistent state (leave at graph defaults).
 *
 * Unwrap > 0.5: treat In as an angle in radians and wrap (In-Prev) into
 * (-pi, pi] before dividing by Dt. Required for Theta_E → ω_e; without it
 * every 2π wrap injects a ~±2π/Dt spike (HybridADRCMPC-4 current spikes).
 */
float d_in = In - Prev;
if (Unwrap > 0.5f) {
    const float pi = 3.14159265359f;
    const float two_pi = 6.28318530718f;
    d_in = fmodf(d_in + pi, two_pi);
    if (d_in < 0.0f) d_in += two_pi;
    d_in -= pi;
}
const float raw = d_in / Dt;
Prev = In;
if (Fc > 0.0f) {
    const float alpha = 1.0f / (1.0f + 1.0f / (6.28318530718f * Fc * Dt));
    OutState += alpha * (raw - OutState);
} else {
    OutState = raw;
}
Out = OutState;
