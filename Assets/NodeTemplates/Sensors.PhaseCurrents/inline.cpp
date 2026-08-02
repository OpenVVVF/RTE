float iu_f = 0.0f;
float iv_f = 0.0f;
float iw_f = 0.0f;
if (platform_get_phase_currents(&iu_f, &iv_f, &iw_f)) {
    /* Keep raw sensor polarity available for general telemetry, while FOC
     * graphs enable inversion for this hardware's current-sensor wiring. */
    const float polarity = InvertPolarity ? -1.0f : 1.0f;
    I_A = rte::Amperes(polarity * iu_f);
    I_B = rte::Amperes(polarity * iv_f);
    I_C = rte::Amperes(polarity * iw_f);
}
