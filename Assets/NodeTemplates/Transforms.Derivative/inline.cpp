/* Filtered derivative: Out ~= d(In)/dt, low-passed at Fc [Hz] to keep
 * sampling noise out of the estimate (Fc <= 0 disables filtering).
 * Prev/OutState are persistent state (leave at graph defaults). */
const float raw = (In - Prev) / Dt;
Prev = In;
if (Fc > 0.0f) {
    const float alpha = 1.0f / (1.0f + 1.0f / (6.28318530718f * Fc * Dt));
    OutState += alpha * (raw - OutState);
} else {
    OutState = raw;
}
Out = OutState;
