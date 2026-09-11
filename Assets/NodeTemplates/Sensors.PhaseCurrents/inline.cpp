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
 * the Gen6 control path, which consumes getPhaseCurrents() unsigned). */
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
