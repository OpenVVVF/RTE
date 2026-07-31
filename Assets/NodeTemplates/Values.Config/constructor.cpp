Cached = platform_config_load(Key, DefaultValue);

/* Only repair motor/encoder physics keys. Never rewrite Ctrl.* / gain keys. */
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
        /* Gen6 FOC default / calibration polarity is -1. A prior bug persisted
         * Sign=+1 into FRAM and left θe wrong (session 220144: id ±40 A).
         * If the graph DefaultValue is -1, force that polarity for bring-up. */
        if (DefaultValue < -0.5f) {
            Cached = -1.0f;
            platform_config_set(Key, Cached);
        } else if (fabsf(Cached) < 0.5f) {
            Cached = (DefaultValue < -0.5f) ? -1.0f : 1.0f;
            platform_config_set(Key, Cached);
        } else {
            Cached = (Cached >= 0.0f) ? 1.0f : -1.0f;
        }
    } else if (DefaultValue > 1.0e-8f && !(Cached > 0.0f)) {
        Cached = DefaultValue;
        platform_config_set(Key, Cached);
    }
}
