#include "Inverter/Control/FaultManager.h"
#include "Inverter/Control/CommandShell.h"
#include "Inverter/Drivers/GateDriver/gate_driver.h"
#include "Inverter/Telemetry.h"

#include "main.h"
#include "tim.h"
#include "adc.h"
#include "usart.h"

namespace Inverter {

/* No C++ code needed in this file; the callbacks below are weak HAL symbols
 * that we override to latch hardware faults. */

} // namespace Inverter

extern "C" {

void SafetyEcc_ReportFault(bool ram, bool flash, uint32_t monitor,
                           uint32_t ram_status, uint32_t flash_status) {
    if (ram) {
        Inverter::FaultManager::instance().raise(
            Inverter::FaultSource::RamEcc, Inverter::FaultReason::RamEccDetected);
    }
    if (flash) {
        Inverter::FaultManager::instance().raise(
            Inverter::FaultSource::FlashEcc, Inverter::FaultReason::FlashEccDetected);
    }
    Telemetry::printf(
        "[FAULT][ECC] ram_monitor=%lu ram_status=0x%08lX flash_status=0x%08lX; reset required",
        static_cast<unsigned long>(monitor),
        static_cast<unsigned long>(ram_status),
        static_cast<unsigned long>(flash_status));
}

/* TIM1 break input (PE15).  The hardware break already disables TIM1 outputs;
 * this callback latches the event so the control loop can shut down cleanly. */
void TIM1_BRK_IRQHandler(void) {
    HAL_TIM_IRQHandler(&htim1);
}

void HAL_TIMEx_BreakCallback(TIM_HandleTypeDef* htim) {
    if (htim != nullptr && htim->Instance == TIM1) {
        /* The gate-driver /FLT pin is active-low and tied to TIM1_BKIN.
         * During boot the gate driver is held in reset, so /FLT may be low
         * and the break flag asserts even though there is no run-time fault.
         * Only latch a fault once the gate driver has left reset and is ready. */
        if (GateDriver_IsReady()) {
            Inverter::FaultManager::instance().raise(
                Inverter::FaultSource::PwmBreak, Inverter::FaultReason::DesatBreak);
        }
        /* One-shot: the NCx5710y /FLT latch holds the break input active
         * until the driver is reset, and a level-retriggered break at this
         * priority starves the main loop (observed on the bench as a full
         * app hang right after enabling TIM_IT_BREAK).  The fault-clear
         * gate-reset path and control start re-arm it. */
        __HAL_TIM_DISABLE_IT(htim, TIM_IT_BREAK);
    }
}

/* ADC HAL errors on ADC1/ADC2 (phase-current sensing path). */
void HAL_ADC_ErrorCallback(ADC_HandleTypeDef* hadc) {
    if (hadc != nullptr && (hadc->Instance == ADC1 || hadc->Instance == ADC2)) {
        Inverter::FaultManager::instance().raise(
            Inverter::FaultSource::AdcError, Inverter::FaultReason::AdcHalError);
    }
}

/* USART3 shell/telemetry transport errors.
 * Suppress warnings during the first 500 ms after reset/flash; the USB/UART
 * bridge and host side often produce a transient framing/noise event while
 * power stabilizes. */
static constexpr uint32_t UART_STARTUP_WINDOW_MS = 500U;

void HAL_UART_ErrorCallback(UART_HandleTypeDef* huart) {
    if (huart != nullptr && huart->Instance == USART3) {
        /* Restart RX so the shell does not stop accepting commands after
         * an overrun/noise/frame error. */
        Inverter::commandShell().recover();

        if (HAL_GetTick() >= UART_STARTUP_WINDOW_MS) {
            Inverter::FaultManager::instance().raise(
                Inverter::FaultSource::UartError, Inverter::FaultReason::UartHalError);
        }
    }
}

} // extern "C"
