Cached = platform_config_load(Key, DefaultValue);

/* Repair zero/invalid motor physics keys from graph defaults.
 * Encoder Sign: keep FRAM if |Sign|>=0.5; otherwise use DefaultValue.
 * Do NOT force polarity — match whatever FOC already spins with. */
const bool is_motor_key =
    (Key != nullptr) &&
    (strstr(Key, "Inductance") != nullptr ||
     strstr(Key, "Resistance") != nullptr ||
     strstr(Key, "FluxLinkage") != nullptr ||
     strstr(Key, "Motor.Poles") != nullptr ||
     strstr(Key, "Encoder.SinCos.Sign") != nullptr ||
     strstr(Key, "Encoder.SinCos.OffsetDeg") != nullptr);

if (is_motor_key) {
    const bool is_sign = (Key != nullptr) &&
                         (strstr(Key, "Encoder.SinCos.Sign") != nullptr);
    if (is_sign) {
        if (fabsf(Cached) < 0.5f) {
            Cached = (DefaultValue < 0.0f) ? -1.0f : 1.0f;
            platform_config_set(Key, Cached);
        } else {
            Cached = (Cached >= 0.0f) ? 1.0f : -1.0f;
        }
    } else if (DefaultValue > 1.0e-8f && !(Cached > 0.0f)) {
        Cached = DefaultValue;
        platform_config_set(Key, Cached);
    }
}
