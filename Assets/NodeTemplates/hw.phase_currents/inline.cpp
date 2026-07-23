// ADC1 injected: U signal CH4 (rank 1), V signal CH3 (rank 2)
// ADC2 injected: U reference CH8 (rank 1), V reference CH7 (rank 2)
// Triggered by TIM1 TRGO at PWM bottom; read in ADC ISR.
static constexpr float ADC_VREF = 3.3f;
static constexpr float DIVIDER = 2.0f / 3.0f;
static constexpr float SENSITIVITY_VA = 1.042e-3f;  // LA37S600
static constexpr float COUNTS_FULL = 65535.0f;

const float scale = (ADC_VREF / COUNTS_FULL) / (DIVIDER * SENSITIVITY_VA);

const uint32_t raw_u_sig = HAL_ADCEx_InjectedGetValue(&hadc1, ADC_INJECTED_RANK_1);
const uint32_t raw_v_sig = HAL_ADCEx_InjectedGetValue(&hadc1, ADC_INJECTED_RANK_2);
const uint32_t raw_u_ref = HAL_ADCEx_InjectedGetValue(&hadc2, ADC_INJECTED_RANK_1);
const uint32_t raw_v_ref = HAL_ADCEx_InjectedGetValue(&hadc2, ADC_INJECTED_RANK_2);

const rte::Current iu_raw = rte::Amperes((static_cast<float>(raw_u_sig) - static_cast<float>(raw_u_ref)) * scale);
const rte::Current iv_raw = rte::Amperes((static_cast<float>(raw_v_sig) - static_cast<float>(raw_v_ref)) * scale);

// Subtract calibrated zero-current offsets.
const rte::Current iu_a = iu_raw - offset_u_a;
const rte::Current iv_a = iv_raw - offset_v_a;

// Measured W from a separate 12-bit ADC sampler. Platform must provide this symbol.
const uint32_t raw_w_12bit = HW_ReadRawWCurrent_12bit();
const float w_counts = static_cast<float>(raw_w_12bit) - offset_w_12bit;
const rte::Current iw_measured_a = w_counts * scale_w_a_per_lsb;

// Calculated W assuming a balanced three-wire load.
const rte::Current iw_calculated_a = rte::Amperes(-(iu_a.in(au::amperes) + iv_a.in(au::amperes)));

i_abc.a = iu_a;
i_abc.b = iv_a;
i_abc.c = iw_measured_a;
