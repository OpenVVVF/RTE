#include "Inverter/Drivers/CAN/FdcanFault.h"
#include "Inverter/Control/FaultManager.h"

#include "fdcan.h"

namespace Inverter {

bool fdcanFaultInit(bool enable_bus_a, bool enable_bus_b) {
    constexpr uint32_t kNotify = FDCAN_IT_BUS_OFF |
                                 FDCAN_IT_ERROR_PASSIVE |
                                 FDCAN_IT_ERROR_LOGGING_OVERFLOW;
    /* Both buses: error interrupts ride the IT0 line, whose NVIC is owned
     * by the CanBus driver.  Only enabled buses are armed: a disabled bus
     * must never raise a CAN fault. */
    if (enable_bus_a &&
        HAL_FDCAN_ActivateNotification(&hfdcan1, kNotify, 0) != HAL_OK) {
        return false;
    }
    if (enable_bus_b &&
        HAL_FDCAN_ActivateNotification(&hfdcan2, kNotify, 0) != HAL_OK) {
        return false;
    }
    return true;
}

} // namespace Inverter

/* TIME_DOMAIN: FDCAN_ERROR_ISR
 *   Error-status and protocol-error callbacks for the CAN peripheral.
 * CODEGEN: Keep fault raising; codegen will add normal CAN message RX/TX ISRs
 *   alongside these error handlers.
 */
extern "C" void HAL_FDCAN_ErrorStatusCallback(FDCAN_HandleTypeDef* hfdcan,
                                              uint32_t ErrorStatusITs) {
    if (hfdcan == nullptr) {
        return;
    }
    if (hfdcan->Instance == FDCAN1 || hfdcan->Instance == FDCAN2) {
        if ((ErrorStatusITs & FDCAN_FLAG_BUS_OFF) != 0) {
            Inverter::FaultManager::instance().raise(
                Inverter::FaultSource::CanBusOff, Inverter::FaultReason::CanBusOff);
        }
        if ((ErrorStatusITs & FDCAN_FLAG_ERROR_PASSIVE) != 0) {
            Inverter::FaultManager::instance().raise(
                Inverter::FaultSource::CanErrorPassive, Inverter::FaultReason::CanErrorPassive);
        }
        if ((ErrorStatusITs & FDCAN_FLAG_ERROR_LOGGING_OVERFLOW) != 0) {
            Inverter::FaultManager::instance().raise(
                Inverter::FaultSource::CanProtocolError,
                Inverter::FaultReason::CanErrorLogOverflow);
        }
    }
}

extern "C" void HAL_FDCAN_ErrorCallback(FDCAN_HandleTypeDef* hfdcan) {
    if (hfdcan != nullptr &&
        (hfdcan->Instance == FDCAN1 || hfdcan->Instance == FDCAN2)) {
        Inverter::FaultManager::instance().raise(
            Inverter::FaultSource::CanProtocolError, Inverter::FaultReason::CanHalError);
    }
}
