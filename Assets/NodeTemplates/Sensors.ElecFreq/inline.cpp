/* Electrical frequency estimate: f_e = |rpm| / 60 * (poles / 2), low-passed.
 * NaN/invalid guards fall back to zero so the sequencer stays in mode 0. */
float poles = Poles;
if (!(poles >= 2.0f)) poles = 10.0f;

float f_raw = fabsf(Omega_Mech) / 60.0f * (poles * 0.5f);
if (!std::isfinite(f_raw)) f_raw = 0.0f;

Dir = (Omega_Mech >= 0.0f) ? 1.0f : -1.0f;

if (Fc > 0.0f && Dt > 0.0f) {
    const float alpha = 1.0f / (1.0f + 1.0f / (6.28318530718f * Fc * Dt));
    State += alpha * (f_raw - State);
} else {
    State = f_raw;
}
F_Elec = State;
