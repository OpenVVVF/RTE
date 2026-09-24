#include "platform_api.h"

#include "Inverter/Drivers/PWM/pwm.h"
#include "Inverter/Calibration/MotorCalibration.h"
#include "Inverter/Drivers/Sensors/ApplicationSensors.h"
#include "Inverter/Drivers/Sensors/PhaseCurrentADC.h"
#include "Inverter/Drivers/Sensors/EncoderADC.h"
#include "Inverter/Drivers/Sensors/DcLinkCurrentSensor.h"
#include "Inverter/Drivers/Sensors/DcLinkVoltageSensor.h"
#include "Inverter/Drivers/Sensors/SampleScheduler.h"
#include "Inverter/Drivers/CAN/CanBus.h"
#include "Inverter/Drivers/Storage/RteParamStore.h"
#include "Inverter/Control/FaultManager.h"
#include "Inverter/Control/CurrentObserver.h"
#include "Inverter/Control/CurrentLoopMath.h"
#include "Inverter/Telemetry.h"
#include "Inverter/Drivers/Logging/TraceRecorder.h"

#include "main.h"
#include "adc.h"
#include "tim.h"

/* --------------------------------------------------------------------------
 * PWM / gate-driver outputs
 * -------------------------------------------------------------------------- */

static volatile bool s_control_outputs_enabled = false;
static bool s_sample_window_valid = true;
static Inverter::CurrentLoopMath::Controller s_vector_pi;

void platform_set_control_outputs_enabled(bool enabled) {
    const uint32_t saved = __get_PRIMASK();
    __disable_irq();
    const bool changed = s_control_outputs_enabled != enabled;
    if (changed) s_vector_pi.reset();
    s_control_outputs_enabled = enabled;
    __set_PRIMASK(saved);
    if (changed) {
        Telemetry::log("control_outputs_enabled", enabled ? 1.0f : 0.0f);
    }
}

bool platform_control_outputs_enabled(void) {
    return s_control_outputs_enabled;
}

void platform_pwm_set(float du, float dv, float dw) {
    if (s_control_outputs_enabled) {
        PWM_SetThreePhaseDuty(du, dv, dw);
    }
}

void platform_pwm_set_voltage_vector(float valpha, float vbeta, float vdc) {
    if (s_control_outputs_enabled) {
        PWM_SetVoltageVector(valpha, vbeta, vdc);
    }
}

/* --------------------------------------------------------------------------
 * Sensor inputs
 * -------------------------------------------------------------------------- */

bool platform_get_phase_currents(float* iu_a, float* iv_a, float* iw_a) {
    if (iu_a == nullptr || iv_a == nullptr || iw_a == nullptr) {
        return false;
    }
    return Inverter::phaseCurrentADC().latest(*iu_a, *iv_a, *iw_a);
}

static bool s_use_observer = false;

void platform_set_use_observer(bool enabled) {
    s_use_observer = enabled;
}

bool platform_get_use_observer(void) {
    return s_use_observer;
}

void platform_get_observer_currents(float* iu_a, float* iv_a, float* iw_a) {
    if (iu_a == nullptr || iv_a == nullptr || iw_a == nullptr) {
        return;
    }
    Inverter::currentObserver().getPhaseCurrents(*iu_a, *iv_a, *iw_a);
}

void platform_observer_predict(float valpha_v, float vbeta_v,
                               float theta_elec_rad, float dt_s) {
    Inverter::currentObserver().predict(valpha_v, vbeta_v,
                                        theta_elec_rad, dt_s);
}

void platform_observer_set_motor_params(float r_ohm, float l_henry,
                                        float flux_linkage_wb,
                                        float pole_pairs) {
    Inverter::currentObserver().setMotorParameters(r_ohm, l_henry,
                                                   flux_linkage_wb, pole_pairs);
}

void platform_observer_init_from_calibration(void) {
    const auto& cal = Inverter::MotorCalibration::instance();
    Inverter::currentObserver().setMotorParameters(
        cal.r_phase_avg,
        cal.ld_henry,
        cal.flux_linkage_wb,
        cal.pole_count * 0.5f);
    Inverter::currentObserver().reset();
}

bool platform_adc_get_burst_sample(float* iu0_a, float* iv0_a,
                                   float* iu1_a, float* iv1_a,
                                   uint32_t* time_us) {
    if (iu0_a == nullptr || iv0_a == nullptr || iu1_a == nullptr || iv1_a == nullptr) {
        return false;
    }
    Inverter::PhaseCurrentADC::BurstSample burst;
    if (!Inverter::phaseCurrentADC().latestBurst(burst)) {
        return false;
    }
    *iu0_a = burst.point[0].iu_a;
    *iv0_a = burst.point[0].iv_a;
    *iu1_a = burst.point[1].iu_a;
    *iv1_a = burst.point[1].iv_a;
    if (time_us != nullptr) {
        *time_us = burst.point[0].time_us;
    }
    return true;
}

void platform_observer_correct(float iu_meas_a, float iv_meas_a,
                               float diudt_a_per_s, float divdt_a_per_s,
                               uint32_t t_us) {
    Inverter::currentObserver().correct(iu_meas_a, iv_meas_a,
                                        diudt_a_per_s, divdt_a_per_s, t_us);
}

uint32_t platform_schedule_adaptive_sample(float duty_u, float duty_v,
                                           float duty_w, uint32_t arr) {
    if (!s_control_outputs_enabled) {
        return 0U;
    }
    /* 12 us total: 6 us on either side for switching settling and ADC burst. */
    const uint32_t min_gap_ticks = static_cast<uint32_t>(12e-6f * 275000000.0f / float(TIM1->PSC + 1U));
    uint32_t ccr4 = 10U;
    uint32_t gap_ticks = 0U;

    if (PWM_FindSafeSamplePoint(duty_u, duty_v, duty_w, arr,
                                min_gap_ticks, &ccr4, &gap_ticks)) {
        // Both extrema are sampled by TIM1 TRGO2. Require both quiet windows.
        const float min_duty = std::min({duty_u, duty_v, duty_w});
        const uint32_t top_gap = uint32_t(2.0f * float(arr) * min_duty / 100.0f);
        gap_ticks = std::min(gap_ticks, top_gap);
        if (gap_ticks < min_gap_ticks + 20U) {
            s_sample_window_valid = false;
            return 0U;
        }
        s_sample_window_valid = true;
        Inverter::sampleScheduler().scheduleNextSample(ccr4, arr);
        return gap_ticks;
    }

    s_sample_window_valid = false;
    Inverter::sampleScheduler().scheduleFallback();
    return 0U;
}

uint32_t platform_pwm_get_arr(void) {
    return __HAL_TIM_GET_AUTORELOAD(&htim1);
}

uint32_t platform_adc_get_injected_u_sig(void) {
    return HAL_ADCEx_InjectedGetValue(&hadc1, ADC_INJECTED_RANK_1);
}

uint32_t platform_adc_get_injected_v_sig(void) {
    return HAL_ADCEx_InjectedGetValue(&hadc1, ADC_INJECTED_RANK_2);
}

uint32_t platform_adc_get_injected_u_ref(void) {
    return HAL_ADCEx_InjectedGetValue(&hadc2, ADC_INJECTED_RANK_1);
}

uint32_t platform_adc_get_injected_v_ref(void) {
    return HAL_ADCEx_InjectedGetValue(&hadc2, ADC_INJECTED_RANK_2);
}

float platform_adc_get_offset_u_a(void) {
    return Inverter::phaseCurrentADC().lastOffsetU();
}

float platform_adc_get_offset_v_a(void) {
    return Inverter::phaseCurrentADC().lastOffsetV();
}

bool platform_get_encoder_angle(float* angle_deg) {
    if (angle_deg == nullptr) {
        return false;
    }
    return Inverter::encoderADC().sample(*angle_deg);
}

float platform_get_encoder_angle_latest(void) {
    /* Extrapolated to the read instant: smooths the sample staircase between
     * the 5 kHz encoder stream and the 10 kHz FOC steps. */
    return Inverter::encoderADC().extrapolatedAngleDeg();
}

float platform_get_motor_rpm(void) {
    return Inverter::encoderADC().rpmMech();
}

float platform_get_dc_link_voltage(void) {
    return Inverter::dcLinkVoltageSensor().voltage();
}

float platform_get_rpm_mech(void) {
    return Inverter::encoderADC().rpmMech();
}

float platform_get_rpm_elec(void) {
    /* Electrical RPM = mechanical RPM * pole pairs, signed so it follows the
     * same convention as the sign-corrected electrical angle used by the
     * generated control graph (ElecAngle applies Motor.Encoder.SinCos.Sign).
     * The raw encoder RPM is in the physical encoder-count direction; if the
     * encoder is mounted opposite to the rotor field, encoder_sign is -1 and
     * the electrical angle increases while the raw encoder counts decrease.
     * Without the sign correction here, feed-forward terms see the wrong speed
     * sign and produce braking/cross-coupling voltages instead of assisting. */
    const auto& cal = Inverter::MotorCalibration::instance();
    const float pole_pairs = cal.pole_count * 0.5f;
    const float encoder_sign = (cal.encoder_sign >= 0.0f) ? 1.0f : -1.0f;
    return Inverter::encoderADC().rpmMech() * pole_pairs * encoder_sign;
}

uint32_t platform_get_encoder_raw_sin(void) {
    return Inverter::encoderADC().lastRawSin();
}

uint32_t platform_get_encoder_raw_cos(void) {
    return Inverter::encoderADC().lastRawCos();
}

float platform_get_dc_link_current(void) {
    return Inverter::dcLinkCurrentSensor().current();
}

float platform_get_dc_link_power(void) {
    return Inverter::dcLinkCurrentSensor().power();
}

/* Read the driver's 20 kHz DMA snapshot directly - never an ad-hoc SPI
 * transaction from the main loop (racing the burst path corrupts frames and
 * can block for the SPI timeout). */
float platform_phase_voltage_u(void) {
    /* MAX22530 AIN3 (index 2) = VSENSE_PH_U_B. */
    return Inverter::dcLinkVoltageSensor().adc().voltage(2) * Inverter::VSENSE_DIVIDER_RATIO;
}

float platform_phase_voltage_v(void) {
    /* MAX22530 AIN2 (index 1) = VSENSE_PH_V_B. */
    return Inverter::dcLinkVoltageSensor().adc().voltage(1) * Inverter::VSENSE_DIVIDER_RATIO;
}

float platform_phase_voltage_w(void) {
    /* MAX22530 AIN1 (index 0) = VSENSE_PH_W_B. */
    return Inverter::dcLinkVoltageSensor().adc().voltage(0) * Inverter::VSENSE_DIVIDER_RATIO;
}

/* --------------------------------------------------------------------------
 * Application sensors — backed by the ApplicationSensors base-image driver.
 *
 * The analog pins are configured in Src/adc.c:
 *   AIN_THROTTLE_A  = PA3 / ADC1_INP15 (shared ADC1/ADC2)
 *   AIN_THROTTLE_B  = PA4 / ADC1_INP18 (shared ADC1/ADC2)
 *   AIN_TMP_SENSE_1 = PF8 / ADC3_INP7
 *   AIN_TMP_SENSE_2 = PA1 / ADC1_INP17
 *   AIN_TMP_SENSE_3 = PA0 / ADC1_INP16
 *   AIN_MOTOR_TMP   = PF4 / ADC3_INP9
 * -------------------------------------------------------------------------- */

float platform_get_throttle_a(void) {
    return Inverter::appSensors().throttleA();
}

float platform_get_throttle_b(void) {
    return Inverter::appSensors().throttleB();
}

bool platform_get_throttle_valid(void) {
    return Inverter::appSensors().throttlePlausible();
}

float platform_get_motor_temperature(void) {
    return Inverter::appSensors().motorTemperatureC();
}

float platform_get_inverter_temperature(uint8_t channel) {
    return Inverter::appSensors().inverterTemperatureC(channel);
}

/* --------------------------------------------------------------------------
 * User digital IO
 *
 * Pins 1..8  -> USER_DIN_1..8  (inputs)
 * Pins 1..4  -> USER_DOUT_1..4 (outputs)
 * Pin  5     -> DEBUG_GREEN_LED, pin 6 -> DEBUG_ORANGE_LED (outputs)
 * -------------------------------------------------------------------------- */

namespace {
struct DioMapEntry {
    GPIO_TypeDef* port;
    uint16_t      pin;
};

constexpr DioMapEntry DIN_MAP[8] = {
    {USER_DIN_1_GPIO_Port, USER_DIN_1_Pin},
    {USER_DIN_2_GPIO_Port, USER_DIN_2_Pin},
    {USER_DIN_3_GPIO_Port, USER_DIN_3_Pin},
    {USER_DIN_4_GPIO_Port, USER_DIN_4_Pin},
    {USER_DIN_5_GPIO_Port, USER_DIN_5_Pin},
    {USER_DIN_6_GPIO_Port, USER_DIN_6_Pin},
    {USER_DIN_7_GPIO_Port, USER_DIN_7_Pin},
    {USER_DIN_8_GPIO_Port, USER_DIN_8_Pin},
};

constexpr DioMapEntry DOUT_MAP[6] = {
    {USER_DOUT_1_GPIO_Port, USER_DOUT_1_Pin},
    {USER_DOUT_2_GPIO_Port, USER_DOUT_2_Pin},
    {USER_DOUT_3_GPIO_Port, USER_DOUT_3_Pin},
    {USER_DOUT_4_GPIO_Port, USER_DOUT_4_Pin},
    {DEBUG_GREEN_LED_GPIO_Port, DEBUG_GREEN_LED_Pin},
    {DEBUG_ORANGE_LED_GPIO_Port, DEBUG_ORANGE_LED_Pin},
};
} // namespace

bool platform_digital_read(uint8_t pin) {
    if (pin < 1 || pin > 8) {
        return false;
    }
    return HAL_GPIO_ReadPin(DIN_MAP[pin - 1].port, DIN_MAP[pin - 1].pin) == GPIO_PIN_SET;
}

void platform_digital_write(uint8_t pin, bool value) {
    if (pin < 1 || pin > 6) {
        return;
    }
    HAL_GPIO_WritePin(DOUT_MAP[pin - 1].port, DOUT_MAP[pin - 1].pin,
                      value ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* --------------------------------------------------------------------------
 * CAN bus
 * -------------------------------------------------------------------------- */

bool platform_can_send(uint8_t bus, uint32_t id, bool ext,
                       const uint8_t* data, uint8_t dlc) {
    /* Graph/shell-facing bus numbering is 1-based (1 = "A"/FDCAN1,
     * 2 = "B"/FDCAN2); the driver is 0-based. */
    if (bus < 1 || bus > Inverter::CanBus::NUM_BUSES) {
        return false;
    }
    return Inverter::canBus().send(bus - 1, id, ext, data, dlc);
}

int platform_can_rx(uint8_t bus, uint32_t id, uint8_t* data, uint32_t* seq_out) {
    if (bus < 1 || bus > Inverter::CanBus::NUM_BUSES) {
        if (seq_out != nullptr) {
            *seq_out = 0;
        }
        return -1;
    }
    Inverter::CanBus::Frame f;
    uint32_t seq = 0;
    if (!Inverter::canBus().rxLatest(bus - 1, id, false, f, &seq)) {
        if (seq_out != nullptr) {
            *seq_out = seq;
        }
        return -1;
    }
    const uint8_t n = f.dlc > 8 ? 8 : f.dlc;
    if (data != nullptr) {
        for (uint8_t i = 0; i < n; ++i) {
            data[i] = f.data[i];
        }
    }
    if (seq_out != nullptr) {
        *seq_out = seq;
    }
    return n;
}

void platform_trace_configure8(
    const char* key0, float scale0, const char* key1, float scale1,
    const char* key2, float scale2, const char* key3, float scale3,
    const char* key4, float scale4, const char* key5, float scale5,
    const char* key6, float scale6, const char* key7, float scale7) {
    const char* keys[8] = {key0, key1, key2, key3, key4, key5, key6, key7};
    const float scales[8] = {scale0, scale1, scale2, scale3,
                             scale4, scale5, scale6, scale7};
    Inverter::traceRecorder().configure8(keys, scales);
}

void platform_trace_capture8(float value0, float value1, float value2, float value3,
                             float value4, float value5, float value6, float value7) {
    Inverter::traceRecorder().capture8(value0, value1, value2, value3,
                                       value4, value5, value6, value7);
}

bool platform_trace_event(uint8_t channel, float value, bool snapshot) {
    return Inverter::traceRecorder().publishEvent(channel, value, snapshot);
}

bool platform_trace_register_event(uint8_t channel, const char* key) {
    return Inverter::traceRecorder().registerEventChannel(channel, key);
}

/* --------------------------------------------------------------------------
 * Safety / faults
 * -------------------------------------------------------------------------- */

void platform_raise_fault(uint32_t source, uint8_t reason) {
    Inverter::FaultManager::instance().raise(
        static_cast<Inverter::FaultSource>(source),
        static_cast<Inverter::FaultReason>(reason));
}

bool platform_has_critical_fault(void) {
    return Inverter::FaultManager::instance().isSeverityActive(
        Inverter::FaultSeverity::Critical);
}

/* --------------------------------------------------------------------------
 * Critical sections
 * -------------------------------------------------------------------------- */

static uint32_t s_critical_depth = 0U;
static uint32_t s_critical_saved_mask = 0U;

void platform_critical_enter(void) {
    const uint32_t saved = __get_PRIMASK();
    __disable_irq();
    if (s_critical_depth++ == 0U) s_critical_saved_mask = saved;
    __DMB();
}

void platform_critical_exit(void) {
    __DMB();
    if (s_critical_depth != 0U && --s_critical_depth == 0U)
        __set_PRIMASK(s_critical_saved_mask);
}

/* --------------------------------------------------------------------------
 * Config / persistence
 * -------------------------------------------------------------------------- */

float platform_config_load(const char* key, float default_value) {
    if (key == nullptr) return default_value;
    float value = default_value;
    if (Inverter::RteParamStore::isReady()) {
        if (!Inverter::RteParamStore::get(key, &value)) {
            /* Not found: warn, then create with default. */
            Telemetry::printf(
                "[RTE] config key '%s' not in FRAM; using default %.4f",
                key, static_cast<double>(default_value));
            Inverter::RteParamStore::set(key, default_value);
            Inverter::RteParamStore::flush();
            value = default_value;
        }
    }
    return value;
}

void platform_config_set(const char* key, float value) {
    if (key == nullptr) return;
    if (Inverter::RteParamStore::isReady()) {
        Inverter::RteParamStore::set(key, value);
        Inverter::RteParamStore::flush();
    }
}

float platform_config_get(const char* key) {
    if (key == nullptr) return 0.0f;
    float value = 0.0f;
    if (Inverter::RteParamStore::isReady()) {
        Inverter::RteParamStore::get(key, &value);
    }
    return value;
}

/* --------------------------------------------------------------------------
 * Telemetry
 * -------------------------------------------------------------------------- */

void platform_telemetry_log_f32(const char* key, float value) {
    Telemetry::log(key, value);
}

/* --------------------------------------------------------------------------
 * Time
 * -------------------------------------------------------------------------- */

uint32_t platform_millis(void) {
    return HAL_GetTick();
}

uint32_t platform_micros(void) {
    /* DWT cycle counter is enabled by Telemetry::init().  Fall back to ms
     * resolution if it is not running. */
    if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) != 0U) {
        return DWT->CYCCNT / (SystemCoreClock / 1000000U);
    }
    return HAL_GetTick() * 1000U;
}

static float s_current_domain_dt = 0.0f;

void platform_set_current_domain_dt(float dt_s) {
    s_current_domain_dt = dt_s;
}

float platform_get_current_domain_dt(void) {
    return s_current_domain_dt;
}

namespace {
struct CurrentFrame {
    float id = 0, iq = 0, theta = 0, ia = 0, ib = 0, ic = 0;
    uint32_t cycles = 0, sequence = 0;
    bool valid = false;
    bool down = false;
};
CurrentFrame s_current_frame, s_previous_frame, s_older_frame, s_latched_frame;
float s_raw_id = 0, s_raw_iq = 0;
uint32_t s_current_callback_cycles = 0;
float s_current_age_us = 0, s_actuation_angle = 0;
float s_id_ref = 0, s_iq_ref = 0, s_control_bus = 0;
Inverter::CurrentLoopMath::Result s_control_result;
struct ControlCaptureSample {
    uint32_t cycles, sequence, timer;
    float id, iq, id_ref, iq_ref, theta, act_angle, age;
    float vd_req, vq_req, vd, vq, int_d, int_q, scale, bus;
    float ia, ib, ic, du, dv, dw, active_u, active_v, active_w;
    float raw_id, raw_iq;
    bool valid;
};
constexpr uint32_t CONTROL_CAPTURE_COUNT = 512;
__attribute__((section(".dma_buffers"), aligned(32)))
ControlCaptureSample s_control_capture[CONTROL_CAPTURE_COUNT];
volatile uint32_t s_capture_count = 0;
volatile bool s_capture_armed = false;
uint32_t s_capture_decimation = 1, s_capture_divider = 0;
}

void platform_current_sample_begin(uint32_t callback_cycles) {
    s_current_callback_cycles = callback_cycles;
}
void platform_publish_current_frame(float id, float iq, float theta,
                                    float ia, float ib, float ic) {
    platform_critical_enter();
    s_older_frame = s_previous_frame;
    s_previous_frame = s_current_frame;
    s_current_frame = {id, iq, theta, ia, ib, ic, s_current_callback_cycles,
                       s_current_frame.sequence + 1U,
                       s_sample_window_valid && std::isfinite(id) && std::isfinite(iq),
                       (TIM1->CR1 & TIM_CR1_DIR) != 0U};
    platform_critical_exit();
}
void platform_latch_current_frame(float* id, float* iq, float* theta,
                                  float* age_us, float* valid, float* sequence) {
    platform_critical_enter();
    s_latched_frame = s_current_frame;
    const CurrentFrame previous = s_previous_frame;
    const CurrentFrame older = s_older_frame;
    platform_critical_exit();
    s_raw_id = s_latched_frame.id; s_raw_iq = s_latched_frame.iq;
    const float interval_s = float(uint32_t(s_latched_frame.cycles - previous.cycles)) /
                             float(SystemCoreClock);
    // Average d/q AFTER transforming each raw sample at its own rotor angle.
    // Opposite zero-vector midpoint errors reversed sign in the bench A/B.
    // Keep raw samples for protection and capture; never average across a gap.
    auto feedback = Inverter::CurrentLoopMath::midpointPair(
        {s_raw_id, s_raw_iq}, {previous.id, previous.iq},
        previous.down != s_latched_frame.down,
        previous.valid && previous.sequence + 1U == s_latched_frame.sequence,
        interval_s, 1.0f / PWM_GetFrequency());
    const float old_interval_s = float(uint32_t(previous.cycles - older.cycles)) / float(SystemCoreClock);
    const bool full_cycle = previous.valid && older.valid &&
        older.sequence + 1U == previous.sequence && previous.sequence + 1U == s_latched_frame.sequence &&
        older.down == s_latched_frame.down && previous.down != s_latched_frame.down &&
        interval_s > 0 && interval_s < 0.75f / PWM_GetFrequency() &&
        old_interval_s > 0 && old_interval_s < 0.75f / PWM_GetFrequency();
    if (full_cycle) feedback = Inverter::CurrentLoopMath::midpointCycle(
        {s_raw_id,s_raw_iq}, {previous.id,previous.iq}, {older.id,older.iq}, true);
    s_latched_frame.id = feedback.d; s_latched_frame.iq = feedback.q;
    Telemetry::log("cg_id_raw_a", s_raw_id);
    Telemetry::log("cg_iq_raw_a", s_raw_iq);
    s_current_age_us = float(uint32_t(DWT->CYCCNT - s_latched_frame.cycles)) /
                       (float(SystemCoreClock) / 1000000.0f);
    *id = s_latched_frame.id; *iq = s_latched_frame.iq; *theta = s_latched_frame.theta;
    *age_us = s_current_age_us;
    *valid = s_latched_frame.valid && s_latched_frame.sequence != 0U &&
             s_current_age_us < 1500000.0f / PWM_GetFrequency() ? 1.0f : 0.0f;
    *sequence = float(s_latched_frame.sequence & 0x00ffffffU);
}
float platform_voltage_limit(float bus_v, float requested_bus_fraction) {
    if (!std::isfinite(bus_v) || bus_v <= 1.0f || !std::isfinite(requested_bus_fraction)) return 0;
    // Reserve at least 6 us on EACH side of the sampling point. At 2.5 kHz
    // this permits <=94% duty span. Physical ceiling remains <= linear SVPWM.
    const float span = std::clamp(1.0f - 4.0f * 6e-6f * PWM_GetFrequency(), 0.0f, 0.95f);
    const float max_fraction = span * 0.57735026919f;
    return bus_v * std::clamp(requested_bus_fraction, 0.0f, max_fraction);
}
void platform_modulate(float alpha_v, float beta_v, float bus_v,
                       float* du, float* dv, float* dw) {
    // Shared physical-voltage boundary; the controller uses the identical bound.
    const auto ab = Inverter::CurrentLoopMath::limit({alpha_v, beta_v},
                     platform_voltage_limit(bus_v, 1.0f));
    const auto duty = Inverter::CurrentLoopMath::modulate(ab, bus_v);
    *du = duty.a; *dv = duty.b; *dw = duty.c;
}
void platform_vector_pi(float id_ref, float iq_ref, float id, float iq,
                        float fd, float fq, float kpd, float kid, float kpq, float kiq,
                        float max_bus_fraction, float valid,
                        float* vd, float* vq, float* rd, float* rq, float* scale) {
    s_id_ref = id_ref; s_iq_ref = iq_ref;
    s_control_bus = platform_get_dc_link_voltage();
    s_control_result = s_vector_pi.step({id_ref-id, iq_ref-iq}, {fd,fq},
        {kpd,kpq}, {kid,kiq}, platform_get_current_domain_dt(),
        platform_voltage_limit(s_control_bus, max_bus_fraction),
        s_control_outputs_enabled && valid > 0.5f);
    *vd=s_control_result.limited.d; *vq=s_control_result.limited.q;
    *rd=s_control_result.requested.d; *rq=s_control_result.requested.q;
    *scale=s_control_result.scale;
}
void platform_control_actuation_angle(float angle_rad) { s_actuation_angle = angle_rad; }
void platform_control_capture_step(float du, float dv, float dw) {
    if (!s_capture_armed || !s_control_outputs_enabled) return;
    if (++s_capture_divider < s_capture_decimation) return;
    s_capture_divider=0;
    const uint32_t n = s_capture_count;
    if (n >= CONTROL_CAPTURE_COUNT) { s_capture_armed=false; return; }
    auto& s = s_control_capture[n];
    s.cycles=DWT->CYCCNT; s.sequence=s_latched_frame.sequence;
    s.timer=TIM1->CNT | ((TIM1->CR1 & TIM_CR1_DIR) ? 0x80000000U : 0U);
    s.id=s_latched_frame.id; s.iq=s_latched_frame.iq;
    s.id_ref=s_id_ref; s.iq_ref=s_iq_ref; s.theta=s_latched_frame.theta;
    s.act_angle=s_actuation_angle; s.age=s_current_age_us;
    s.vd_req=s_control_result.requested.d; s.vq_req=s_control_result.requested.q;
    s.vd=s_control_result.limited.d; s.vq=s_control_result.limited.q;
    s.int_d=s_control_result.integral.d; s.int_q=s_control_result.integral.q;
    s.scale=s_control_result.scale; s.bus=s_control_bus;
    s.ia=s_latched_frame.ia; s.ib=s_latched_frame.ib; s.ic=s_latched_frame.ic;
    s.du=du; s.dv=dv; s.dw=dw;
    PWM_GetTrackedActiveDuties(&s.active_u,&s.active_v,&s.active_w);
    s.valid=s_latched_frame.valid;
    s.raw_id=s_raw_id; s.raw_iq=s_raw_iq;
    __DMB(); s_capture_count=n+1U;
    if (n+1U==CONTROL_CAPTURE_COUNT) s_capture_armed=false;
}
void platform_control_capture_arm(uint32_t decimation) {
    platform_critical_enter();
    s_capture_armed=false; s_capture_count=0; s_capture_divider=0;
    s_capture_decimation=std::clamp<uint32_t>(decimation,1U,50U);
    __DMB(); s_capture_armed=true;
    platform_critical_exit();
    Telemetry::printf("[SHELL] ctrlcap armed: max=512 decimation=%lu", (unsigned long)s_capture_decimation);
}
void platform_control_capture_dump(uint32_t offset, uint32_t count) {
    const bool armed=s_capture_armed;
    const uint32_t available=s_capture_count;
    Telemetry::printf("[SHELL] ctrlcap v1 armed=%u count=%lu clock_hz=%lu decimation=%lu",
        unsigned(armed), (unsigned long)available, (unsigned long)SystemCoreClock,
        (unsigned long)s_capture_decimation);
    if (armed || count==0 || offset>=available) return;
    count=std::min<uint32_t>({count,16U,available-offset});
    // Two bounded CSV lines per frame; timestamps are raw DWT cycles. Subtract
    // unsigned cycle values before conversion. 'active' duties are software
    // tracked at timer updates, not measured gate waveforms.
    Telemetry::printf("[SHELL] ccA:index,cycles,seq,id,iq,idref,iqref,theta,actangle,age_us,vdreq,vqreq,vd,vq,intd,intq,scale,bus,raw_id,raw_iq");
    Telemetry::printf("[SHELL] ccB:index,timer,valid,ia,ib,ic,du,dv,dw,active_u,active_v,active_w");
    for(uint32_t i=offset;i<offset+count;++i) {
        const auto& s=s_control_capture[i];
        Telemetry::printf("[SHELL] ccA:%lu,%lu,%lu,%.3f,%.3f,%.3f,%.3f,%.5f,%.5f,%.2f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.5f,%.3f,%.3f,%.3f",
            (unsigned long)i,(unsigned long)s.cycles,(unsigned long)s.sequence,
            double(s.id),double(s.iq),double(s.id_ref),double(s.iq_ref),double(s.theta),
            double(s.act_angle),double(s.age),double(s.vd_req),double(s.vq_req),
            double(s.vd),double(s.vq),double(s.int_d),double(s.int_q),double(s.scale),double(s.bus),double(s.raw_id),double(s.raw_iq));
        Telemetry::printf("[SHELL] ccB:%lu,%lu,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f",
            (unsigned long)i,(unsigned long)s.timer,unsigned(s.valid),double(s.ia),double(s.ib),double(s.ic),
            double(s.du),double(s.dv),double(s.dw),double(s.active_u),double(s.active_v),double(s.active_w));
    }
    Telemetry::printf("[SHELL] ctrlcap page end next=%lu", (unsigned long)(offset+count));
}
