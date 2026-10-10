/* Stateless pedal map: speed moves neutral; there is no temporal slew.
 * Positive Iq retains the graph's forward drive direction. Regen opposes
 * sign-corrected electrical rotation and fades out before standstill. */
Iq = rte::Amperes(0.0f);
Neutral = 0.0f;
RegenAvailable = rte::Amperes(0.0f);
const float drive_max = MaxAmps.in(au::amperes);
const float regen_max = RegenAmps.in(au::amperes);
const bool settings_valid =
    isfinite(drive_max) && drive_max >= 0.0f &&
    isfinite(regen_max) && regen_max >= 0.0f &&
    isfinite(NeutralMax) && NeutralMax >= 0.0f && NeutralMax < 1.0f &&
    isfinite(RegenStartRpm) && RegenStartRpm >= 0.0f &&
    isfinite(NeutralFullRpm) && NeutralFullRpm > RegenStartRpm &&
    isfinite(RegenFullRpm) && RegenFullRpm > RegenStartRpm;
if (Valid && isfinite(A) && isfinite(RpmMech) && isfinite(RpmElec) && settings_valid) {
    const float pedal = fminf(1.0f, fmaxf(0.0f, A));
    const float speed = fabsf(RpmMech);
    const float neutral_scale = fminf(1.0f, fmaxf(0.0f,
        (speed - RegenStartRpm) / (NeutralFullRpm - RegenStartRpm)));
    const float regen_scale = fminf(1.0f, fmaxf(0.0f,
        (speed - RegenStartRpm) / (RegenFullRpm - RegenStartRpm)));
    Neutral = NeutralMax * neutral_scale;
    const float available = (Neutral > 0.0f && RpmElec != 0.0f)
        ? regen_max * regen_scale : 0.0f;
    RegenAvailable = rte::Amperes(available);
    float request = 0.0f;
    if (pedal > Neutral) {
        request = drive_max * (pedal - Neutral) / (1.0f - Neutral);
    } else if (pedal < Neutral) {
        request = -copysignf(available * (Neutral - pedal) / Neutral, RpmElec);
    }
    Iq = rte::Amperes(request);
}
