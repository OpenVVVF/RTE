// ADC1 injected: U signal CH4 (rank 1), V signal CH3 (rank 2)
// ADC2 injected: U reference CH8 (rank 1), V reference CH7 (rank 2)
// Triggered by TIM1 TRGO at PWM bottom; read in ADC ISR.
static constexpr float ADC_VREF = 3.3f;
static constexpr float DIVIDER = 2.0f / 3.0f;
static constexpr float SENSITIVITY_VA = 1.042e-3f;  // LA37S600
static constexpr float COUNTS_FULL = 65535.0f;

const float scale = (ADC_VREF / COUNTS_FULL) / (DIVIDER * SENSITIVITY_VA);

const uint32_t raw_u_sig = platform_adc_get_injected_u_sig();
const uint32_t raw_v_sig = platform_adc_get_injected_v_sig();
const uint32_t raw_u_ref = platform_adc_get_injected_u_ref();
const uint32_t raw_v_ref = platform_adc_get_injected_v_ref();

const rte::Current iu_raw = rte::Amperes((static_cast<float>(raw_u_sig) - static_cast<float>(raw_u_ref)) * scale);
const rte::Current iv_raw = rte::Amperes((static_cast<float>(raw_v_sig) - static_cast<float>(raw_v_ref)) * scale);

// Subtract calibrated zero-current offsets from the base image's startup calibration.
const rte::Current calibrated_offset_u = rte::Amperes(platform_adc_get_offset_u_a());
const rte::Current calibrated_offset_v = rte::Amperes(platform_adc_get_offset_v_a());
const rte::Current iu_a = iu_raw - calibrated_offset_u;
const rte::Current iv_a = iv_raw - calibrated_offset_v;

// W is computed from the two measured phases (three-wire balanced load).
const rte::Current iw_a = rte::Amperes(-(iu_a.in(au::amperes) + iv_a.in(au::amperes)));

/* The phase-current sensors on this hardware are wired with inverted polarity
 * relative to the FOC convention.  Negate all three so downstream transforms
 * see the correct sign (matches base-image FocControlManager). */
i_abc.a = -iu_a;
i_abc.b = -iv_a;
i_abc.c = -iw_a;
