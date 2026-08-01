/* Phase currents with median-of-3 spike rejection.
 *
 * The phase-current ADC samples once per carrier period at the OC4 trigger,
 * which can land on a switching transient - at low carriers (1.5 kHz) the
 * resulting single-sample glitches (tens of amps, alternating sign) drove
 * the current loop into oscillation and a ~76 A spike that stalled the
 * motor.  A median of the last three samples rejects any single-sample
 * glitch at the cost of a one-sample delay. */
float iu_f = 0.0f;
float iv_f = 0.0f;
float iw_f = 0.0f;
if (platform_get_phase_currents(&iu_f, &iv_f, &iw_f)) {
    /* Keep raw sensor polarity available for general telemetry, while FOC
     * graphs enable inversion for this hardware's current-sensor wiring. */
    const float polarity = InvertPolarity ? -1.0f : 1.0f;
    Hu2 = Hu1; Hu1 = Hu0; Hu0 = polarity * iu_f;
    Hv2 = Hv1; Hv1 = Hv0; Hv0 = polarity * iv_f;
    Hw2 = Hw1; Hw1 = Hw0; Hw0 = polarity * iw_f;
}

auto med3 = [](float a, float b, float c) -> float {
    if (a > b) { const float t = a; a = b; b = t; }
    if (b > c) { const float t = b; b = c; c = t; }
    if (a > b) { const float t = a; a = b; b = t; }
    return b;
};

I_A = rte::Amperes(med3(Hu0, Hu1, Hu2));
I_B = rte::Amperes(med3(Hv0, Hv1, Hv2));
I_C = rte::Amperes(med3(Hw0, Hw1, Hw2));
