#include "Inverter/InverterMain.h"
#include "Inverter/AppState.h"
#include "Inverter/LoopStats.h"
#include "Inverter/Telemetry.h"
#include "Inverter/Calibration/AutoCalibrationCoordinator.h"
#include "Inverter/Calibration/CalKvStore.h"
#include "Inverter/Calibration/PoleCalibrator.h"
#include "Inverter/Calibration/EncoderOffsetCalibrator.h"
#include "Inverter/Calibration/ResistanceCalibrator.h"
#include "Inverter/Calibration/InductanceCalibrator.h"
#include "Inverter/Calibration/FluxLinkageCalibrator.h"
#include "Inverter/Control/FaultManager.h"
#include "Inverter/Control/FocControlManager.h"
#include "Inverter/Control/CommandShell.h"
#include "Inverter/Control/ControlSupervisor.h"
#include "Inverter/Control/OpenLoopController.h"
#include "Inverter/Drivers/Sensors/ApplicationSensors.h"
#include "Inverter/Drivers/Sensors/CurrentSensorTest.h"
#include "Inverter/Drivers/Sensors/PhaseCurrentADC.h"
#include "Inverter/Drivers/Sensors/DcLinkVoltageSensor.h"
#include "Inverter/Drivers/Sensors/DcLinkCurrentSensor.h"
#include "Inverter/Drivers/Sensors/EncoderADC.h"
#include "Inverter/Drivers/CAN/CanBus.h"
#include "Inverter/Drivers/CAN/CanSession.h"
#include "Inverter/Drivers/CAN/FdcanFault.h"
#include "Inverter/Drivers/Logging/SupplyMonitor.h"
#include "Inverter/Drivers/Storage/RteParamStore.h"
#include "Inverter/Drivers/PWM/pwm.h"
#include "Inverter/Drivers/GateDriver/gate_driver.h"
#include "Inverter/RteParams.h"

#include "main.h"
#include "spi.h"
#include "cy15b102q_driver.h"
#include "ontime_logger.h"

/* Global RTE codegen state variable.  Referenced by app::<DomainTitle>Init/Step
 * calls inserted at // RTE_EMIT markers. */
AppState appState;

namespace InverterMain {

static CY15B102Q_HandleTypeDef g_fram = {
    .hspi      = &hspi4,
    .cs_port   = FRAM_CS_GPIO_Port,
    .cs_pin    = FRAM_CS_Pin,
    .wp_port   = FRAM_WP_GPIO_Port,
    .wp_pin    = FRAM_WP_Pin,
    .hold_port = FRAM_HOLD_GPIO_Port,
    .hold_pin  = FRAM_HOLD_Pin,
};

extern "C" void CY15B102Q_FaultCallback(CY15B102Q_FaultCode code) {
    Inverter::FaultReason reason = Inverter::FaultReason::FramCommandFailed;
    switch (code) {
        case CY15B102Q_FAULT_INIT_ID_MISMATCH:
            reason = Inverter::FaultReason::FramInitIdMismatch;
            break;
        case CY15B102Q_FAULT_READ_FAILED:
            reason = Inverter::FaultReason::FramReadFailed;
            break;
        case CY15B102Q_FAULT_WRITE_FAILED:
            reason = Inverter::FaultReason::FramWriteFailed;
            break;
        case CY15B102Q_FAULT_COMMAND_FAILED:
        default:
            reason = Inverter::FaultReason::FramCommandFailed;
            break;
    }
    Inverter::FaultManager::instance().raise(
        Inverter::FaultSource::FramComm, reason);
}

/* TIME_DOMAIN: APPLICATION_INIT
 *   One-time setup of base-image services (storage, telemetry, gate driver,
 *   current sense, shell).
 * CODEGEN: Add application-specific initialization here: throttle calibration,
 *   temperature sensor setup, CAN filter/message mapping, control tuning, etc.
 */
static void init()
{
    /* Initialize F-RAM for persistent on-time logging and parameter storage. */
    if (CY15B102Q_Init(&g_fram) == HAL_OK) {
        OnTime_Init(&g_fram);
        Inverter::RteParamStore::init(&g_fram);

        /* Restore learned encoder envelope bounds (written by calibration) so
         * the angle decoder is correct from boot without re-running cal. */
        Inverter::CalKvStore::loadEncoderBounds();

        /* Populate the runtime motor calibration from the KV store so no
         * code path ever falls back to the debug-default angle/sign. */
        Inverter::CalKvStore::loadMotorCalibration();
    }

    /* Telemetry over the MCP2221A USB-UART bridge (USART3). */
    Telemetry::init();
    Telemetry::set_period_us(10000);  /* 100 Hz data frames */

    /* Supply rail monitoring (PVD/AVD/VOSRDY). */
    (void)Inverter::supplyMonitorInit();

    /* Enable the gate-driver power rail early so the isolated current sensors
     * and their references settle with the final supply configuration before
     * the zero-current offset is captured.  Keep the gate driver in reset so
     * the outputs stay disabled. */
    HAL_GPIO_WritePin(GATE_DRIVER_POWER_ENABLE_GPIO_Port,
                      GATE_DRIVER_POWER_ENABLE_Pin,
                      GPIO_PIN_SET);
    HAL_GPIO_WritePin(GATE_DRIVER_RESET_GPIO_Port,
                      GATE_DRIVER_RESET_Pin,
                      GPIO_PIN_RESET);
    HAL_Delay(500);

    /* Phase-current sensor test harness. */
    Inverter::CurrentSensorTest_Init();

    /* DC-link current sensor: start the zero-offset capture only now, after
     * the gate-driver/isolated rails have settled with PWM running. */
    Inverter::dcLinkCurrentSensor().init();

    /* UART command shell for start/stop/freq/mod. */
    Inverter::commandShell().init();

    /* Enable the peripheral power rail that supplies the isolated ADC (VDDPL).
     * CurrentSensorTest_Init() also turns this on, but make sure it is high
     * before the isolated voltage sensor initializes. */
    HAL_GPIO_WritePin(PERIPHERAL_POWER_ENABLE_GPIO_Port,
                      PERIPHERAL_POWER_ENABLE_Pin,
                      GPIO_PIN_SET);
    HAL_Delay(200);

    /* Isolated high-voltage DC-link sensor on SPI2 (VSENSE_ISO_ADC_INTERRUPT = PD1). */
    Inverter::dcLinkVoltageSensor().init();

    /* Slow application sensors (temperatures now, throttle in a later stage).
     * ADC3 free-runs; the update() call in loop() only harvests completed
     * conversions and never blocks. */
    Inverter::appSensors().init();

    /* CAN buses (KV enables; no-op when both disabled). */
    Inverter::canBus().init();

    /* CAN session protocol (KV Can.Proto.*; no-op unless enabled). */
    Inverter::canSession().init();

    /* RTE codegen: initialize all generated timing domains after base-image
     * hardware and services are ready. */
    // RTE_EMIT: app_loop init
    // RTE_EMIT: tim_isr init
    // RTE_EMIT: adc_isr init
    // RTE_EMIT: vsense init

    /* The supervisor owns gate-driver sequencing and PWM start/stop for the
     * generated control loop.  It does NOT start PWM at boot; use the shell
     * command 'control start' when ready. */
    Inverter::ControlSupervisor::instance().init();
}

/* TIME_DOMAIN: APPLICATION_MAIN_LOOP
 *   Rate: free-running, effectively ~100 Hz depending on loop workload.
 *   Soft real-time.  This is where codegen application-layer logic lives:
 *   throttle processing, temperature monitoring, CAN command dispatch,
 *   supervisor/state-machine updates, etc.
 * CODEGEN: Insert application-layer functions here; keep safety/fault handling
 *   in the base image and call it from this loop.
 */
static void loop()
{
    static uint32_t s_last_ontime_ms = 0;

    /* Per-domain rate instrumentation: permanent canary for anything that
     * degrades loop throughput (a blocking driver shows up here immediately).
     * ISR domains are hardware-timed; a starving ISR shows up as app_loop
     * collapsing instead. */
    static uint32_t s_last_hz_ms = 0;
    ++Inverter::LoopStats::app_loop;

    const uint32_t now_ms = HAL_GetTick();

    if ((now_ms - s_last_hz_ms) >= 1000U) {
        Telemetry::log("hz_app_loop", static_cast<float>(Inverter::LoopStats::app_loop));
        Telemetry::log("hz_vsense", static_cast<float>(Inverter::LoopStats::vsense));
        Telemetry::log("hz_tim_isr", static_cast<float>(Inverter::LoopStats::tim_isr));
        Telemetry::log("hz_adc_isr", static_cast<float>(Inverter::LoopStats::adc_isr));
        Inverter::LoopStats::app_loop = 0;
        Inverter::LoopStats::vsense = 0;
        Inverter::LoopStats::tim_isr = 0;
        Inverter::LoopStats::adc_isr = 0;
        s_last_hz_ms = now_ms;
    }

    /* RTE codegen: application-domain step (throttle, temperature, CAN command
     * dispatch, state machines, etc.).  Runs at main-loop cadence. */
    // RTE_EMIT: app_loop step

    /* Voltage-sense domain step (MAX22530 phase + DC-link voltages). */
    ++Inverter::LoopStats::vsense;
    // RTE_EMIT: vsense step

    /* TIME_DOMAIN: HOUSEKEEPING_1HZ
     *   Persistent on-time counter and boot count.  Non-volatile storage write.
     * CODEGEN: Add other slow housekeeping here (e.g., SOH logging, thermal models).
     */
    if ((now_ms - s_last_ontime_ms) >= 1000U) {
        OnTime_Update();
        s_last_ontime_ms = now_ms;
    }

    /* Isolated DC-link voltage sensor: sample on every loop so the logged
     * value is always the latest conversion from the EXTI ISR. */
    Inverter::dcLinkVoltageSensor().update();
    Inverter::dcLinkCurrentSensor().update();
    Inverter::supplyMonitorUpdate();

    /* Application sensors (temps/throttle): harvest + recompute; never blocks. */
    Inverter::appSensors().update();

    /* CAN: drain TX queues, bus-off recovery, session heartbeat watch. */
    Inverter::canBus().update();
    Inverter::canSession().update();

    /* Encoder: RPM estimate + signal-quality fault evaluation (the DMA ISR
     * only decodes and publishes the angle snapshot). */
    Inverter::encoderADC().diagnose();

    /* Phase-current sensors: reference-channel plausibility (armed after
     * first healthy sighting, so boot rail settle can't false-trip). */
    Inverter::phaseCurrentADC().diagnose();

    /* Calibration machinery: pump the open-loop controller and every
     * calibrator state machine.  All early-out when inactive. */
    Inverter::openLoopController().update();
    Inverter::autoCalibrationCoordinator().update();
    Inverter::poleCalibrator().update();
    Inverter::encoderOffsetCalibrator().update();
    Inverter::resistanceCalibrator().update();
    Inverter::inductanceCalibrator().update();
    Inverter::fluxLinkageCalibrator().update();

    /* Legacy FOC manager (forced-angle diagnostics, offset experiments). */
    Inverter::focControlManager().update();

    /* TIME_DOMAIN: SAFETY_SUPERVISOR_100HZ
     *   Fault logging, safety actions, command dispatch.
     * CODEGEN: Add application-level state machine / command handlers here.
     *   The base image handles shutdown on Critical faults.
     */
    Inverter::ControlSupervisor::instance().service();
    Inverter::FaultManager::instance().service();
    Inverter::commandShell().poll();
    Inverter::FaultManager::instance().executeSafetyActions();

    /* Flush queued telemetry values over UART. */
    Telemetry::updateSensors();
}

} // namespace InverterMain

extern "C" void InverterMain_Run(void)
{
    InverterMain::init();
    while (1) {
        InverterMain::loop();
    }
}
