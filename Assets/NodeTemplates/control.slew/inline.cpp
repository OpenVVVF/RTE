/* Rate-limited reference follower: Out tracks In at max Rate units/sec.
 * On control start Value resets to the graph default, so references ramp
 * smoothly from zero instead of stepping. */
const float target = In.in(au::amperes);
const float step = Rate * Dt;
float v = Value.in(au::amperes);
if (target > v + step) {
    v += step;
} else if (target < v - step) {
    v -= step;
} else {
    v = target;
}
Value = rte::Amperes(v);
Out = Value;
