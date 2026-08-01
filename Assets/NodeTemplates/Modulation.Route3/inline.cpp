/* Duty routing for the modulation ladder: out = (1-Blend)*A + Blend*B, then
 * sanitize.  A non-finite duty (NaN/inf from any upstream path) becomes the
 * 50% neutral point so PwmOut can never emit NaN. */
auto san = [](float x) -> float { return std::isfinite(x) ? x : 50.0f; };
const float w = (Blend < 0.0f) ? 0.0f : ((Blend > 1.0f) ? 1.0f : Blend);

Duty_A = san((1.0f - w) * A_A + w * B_A);
Duty_B = san((1.0f - w) * A_B + w * B_B);
Duty_C = san((1.0f - w) * A_C + w * B_C);
