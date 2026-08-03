/* Micro-burst phase-current sampling:
 *   ADC1 injected: [U_sig, V_sig, U_sig, V_sig] (ranks 1..4)
 *   ADC2 injected: [U_ref, V_ref, U_ref, V_ref] (ranks 1..4)
 * Average the two points per phase for a cleaner sample. */
float iu0 = 0.0f, iv0 = 0.0f, iu1 = 0.0f, iv1 = 0.0f;
uint32_t burst_time_us = 0;
if (platform_adc_get_burst_sample(&iu0, &iv0, &iu1, &iv1, &burst_time_us)) {
    const float iu_avg = 0.5f * (iu0 + iu1);
    const float iv_avg = 0.5f * (iv0 + iv1);

    /* W is computed from the two measured phases (three-wire balanced load). */
    const float iw_avg = -(iu_avg + iv_avg);

    /* The phase-current sensors on this hardware are wired with inverted
     * polarity relative to the FOC convention.  Negate all three so
     * downstream transforms see the correct sign. */
    I_A = rte::Amperes(-iu_avg);
    I_B = rte::Amperes(-iv_avg);
    I_C = rte::Amperes(-iw_avg);
}
