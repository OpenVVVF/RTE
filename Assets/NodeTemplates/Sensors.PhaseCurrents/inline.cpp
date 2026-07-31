float iu_f = 0.0f;
float iv_f = 0.0f;
float iw_f = 0.0f;
if (platform_get_phase_currents(&iu_f, &iv_f, &iw_f)) {
    /* The phase-current sensors on this hardware are wired with inverted
     * polarity relative to the FOC convention.  Negate all three so
     * downstream transforms see the correct sign (matches the base-image
     * FocControlManager). */
    I_A = rte::Amperes(-iu_f);
    I_B = rte::Amperes(-iv_f);
    I_C = rte::Amperes(-iw_f);
}
