/* use_observer (Gen6 platform_api.h): when set, this node outputs the
 * CurrentObserver's predicted/corrected currents instead of the raw ADC
 * samples, exactly like Gen6 FocControlManager::onPwmPeriod() switching its
 * feedback source.  The platform API has no observer-valid accessor
 * (platform_get_observer_currents is void), so the flag alone selects the
 * source — before the first observer correction the observer reports zeros,
 * same as Gen6.
 *
 * Polarity: the inversion trim applies to the raw ADC path only.  The
 * currents that correct the observer are polarity-corrected before they
 * reach it, so observer output already follows the FOC convention (matches
 * the Gen6 control path, which consumes getPhaseCurrents() unsigned).
 *
 * Burst slope/timestamp: the latest micro-burst (the same latched
 * conversion the single-point read reports) also yields measured-current
 * slopes, estimated as the finite difference between successive bursts:
 *   Diudt = (iu_burst - PrevIuA) / (burst_us - PrevBurstUs)
 * The slopes describe the measured currents, so the InvertPolarity trim is
 * applied to them exactly like the raw-path currents.  BurstTimeUs carries
 * the burst's own timestamp from platform_adc_get_burst_sample; until two
 * bursts have been seen the slopes read 0.  The observer's correct()
 * consumes but does not require these values — its correction math uses
 * only the current levels; it stores the slopes for the RLS estimator. */
float iu0 = 0.0f;
float iv0 = 0.0f;
float iu1 = 0.0f;
float iv1 = 0.0f;
uint32_t burst_time_us = 0;
if (platform_adc_get_burst_sample(&iu0, &iv0, &iu1, &iv1, &burst_time_us)) {
    const float polarity = InvertPolarity ? -1.0f : 1.0f;
    const float iu_burst = polarity * 0.5f * (iu0 + iu1);
    const float iv_burst = polarity * 0.5f * (iv0 + iv1);
    const float prev_us = PrevBurstUs;
    const uint32_t dt_us = burst_time_us - static_cast<uint32_t>(prev_us);  /* wraps correctly */
    if (prev_us > 0.0f && dt_us > 0u) {
        const float dt_rcp = 1.0e6f / static_cast<float>(dt_us);
        Diudt = (iu_burst - PrevIuA) * dt_rcp;
        Divdt = (iv_burst - PrevIvA) * dt_rcp;
    } else {
        Diudt = 0.0f;
        Divdt = 0.0f;
    }
    PrevIuA = iu_burst;
    PrevIvA = iv_burst;
    PrevBurstUs = static_cast<float>(burst_time_us);
    BurstTimeUs = static_cast<float>(burst_time_us);
}

if (platform_get_use_observer()) {
    float iu_o = 0.0f;
    float iv_o = 0.0f;
    float iw_o = 0.0f;
    platform_get_observer_currents(&iu_o, &iv_o, &iw_o);
    I_A = rte::Amperes(iu_o);
    I_B = rte::Amperes(iv_o);
    I_C = rte::Amperes(iw_o);
} else {
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
}
