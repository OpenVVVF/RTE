/* Current observer node:
 *   1. Correct the observer with the measured phase currents from the
 *      hw.phase_currents node (ADC ISR domain).
 *   2. Run the Luenberger prediction step using the applied voltage vector.
 *   3. Output the observer-estimated phase currents.
 *
 * The correction uses the latest micro-burst measurement.  The prediction
 * uses the voltage vector applied for the next control period.
 */
const float iu_meas = I_A_Meas.in(au::amperes);
const float iv_meas = I_B_Meas.in(au::amperes);
const float diudt = Diudt;
const float divdt = Divdt;
const uint32_t burst_time_us = static_cast<uint32_t>(BurstTimeUs);

platform_observer_correct(iu_meas, iv_meas, diudt, divdt, burst_time_us);

/* The input voltage is already delayed by one control step via the
 * math.delay node, breaking the algebraic loop. */
platform_observer_predict(V_Alpha.in(au::volts), V_Beta.in(au::volts),
                          ThetaElec, platform_get_current_domain_dt());

float iu_obs = 0.0f, iv_obs = 0.0f, iw_obs = 0.0f;
platform_get_observer_currents(&iu_obs, &iv_obs, &iw_obs);

I_A = rte::Amperes(iu_obs);
I_B = rte::Amperes(iv_obs);
I_C = rte::Amperes(iw_obs);
