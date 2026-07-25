float iu_f = 0.0f;
float iv_f = 0.0f;
float iw_f = 0.0f;
if (platform_get_phase_currents(&iu_f, &iv_f, &iw_f)) {
    I_U = rte::Amperes(iu_f);
    I_V = rte::Amperes(iv_f);
    I_W = rte::Amperes(iw_f);
}
