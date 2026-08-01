/* Rate-limited reference follower: Out tracks In at max Rate units/sec.
 * On control start Value resets to the graph default, so references ramp
 * smoothly from zero instead of stepping.
 * Sample period follows the live control-ISR rate (tracks the carrier);
 * falls back to the Dt parameter. */
float dt = platform_get_pwm_dt();
if (!(dt > 0.0f)) dt = Dt;

const float target = In;
const float step = Rate * dt;
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
