/* Rate-limited reference follower: Out tracks In at max Rate units/sec.
 * On control start Value resets to the graph default, so references ramp
 * smoothly from zero instead of stepping. */
const float target = In;
const float step = Rate * Dt;
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
