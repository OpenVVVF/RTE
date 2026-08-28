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
#include "Inverter/Telemetry.h"
#include "Inverter/Drivers/Logging/TraceRecorder.h"

#include "main.h"
#include "adc.h"
#include "tim.h"

/* --------------------------------------------------------------------------
 * PWM / gate-driver outputs
 * -------------------------------------------------------------------------- */

void platform_pwm_set(float du, float dv, float dw) {
    PWM_SetThreePhaseDuty(du, dv, dw);
}

void platform_pwm_set_voltage_vector(float valpha, float vbeta, float vdc) {
    PWM_SetVoltageVector(valpha, vbeta, vdc);
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
    /* 6 us at 275 MHz timer clock: deadtime + switching settling + ADC burst. */
    const uint32_t min_gap_ticks = 1650U;
    uint32_t ccr4 = 10U;
    uint32_t gap_ticks = 0U;

    if (PWM_FindSafeSamplePoint(duty_u, duty_v, duty_w, arr,
                                min_gap_ticks, &ccr4, &gap_ticks)) {
        Inverter::sampleScheduler().scheduleNextSample(ccr4, arr);
        return gap_ticks;
    }

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
    return Inverter::dcLinkVoltageSensor().adc().voltage(0);
}

float platform_phase_voltage_v(void) {
    return Inverter::dcLinkVoltageSensor().adc().voltage(1);
}

float platform_phase_voltage_w(void) {
    return Inverter::dcLinkVoltageSensor().adc().voltage(2);
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

void platform_critical_enter(void) {
    __disable_irq();
}

void platform_critical_exit(void) {
    __enable_irq();
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
