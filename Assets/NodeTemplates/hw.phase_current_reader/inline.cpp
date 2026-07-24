float iu_f = 0.0f;
float iv_f = 0.0f;
float iw_f = 0.0f;
if (platform_get_phase_currents(&iu_f, &iv_f, &iw_f)) {
    iu = rte::Amperes(iu_f);
    iv = rte::Amperes(iv_f);
    iw = rte::Amperes(iw_f);
}
