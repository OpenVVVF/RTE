/* Rate-limited reference follower: Out tracks In at max Rate units/sec.
 * On control start Value resets to the graph default, so references ramp
 * smoothly from zero instead of stepping.
 *
 * dt comes from the live control rate (platform_get_control_dt) so Rate
 * stays in true units/sec when the carrier changes at runtime; the Dt
 * parameter is kept for backward compatibility but ignored. */
const float step = Rate * platform_get_control_dt();
const float target = In;
float v = Value;
if (target > v + step) {
    v += step;
} else if (target < v - step) {
    v -= step;
} else {
    v = target;
}
Value = v;
Out = Value;
