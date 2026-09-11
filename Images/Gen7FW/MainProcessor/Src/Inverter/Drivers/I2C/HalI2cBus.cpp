#include "Inverter/Drivers/I2C/HalI2cBus.h"

namespace Inverter {

bool HalI2cBus::transfer(uint8_t addr7,
                         const uint8_t* tx, uint8_t tx_len,
                         uint8_t* rx, uint8_t rx_len,
                         uint32_t timeout_ms) {
    if ((tx == nullptr) || (tx_len == 0)) {
        return false;
    }
    HAL_StatusTypeDef st;
    if ((rx != nullptr) && (rx_len > 0)) {
        /* tx[0] = register pointer, then repeated-start read. */
        st = HAL_I2C_Mem_Read(m_hi2c, static_cast<uint16_t>(addr7) << 1, tx[0],
                              I2C_MEMADD_SIZE_8BIT, rx, rx_len, timeout_ms);
    } else {
        /* tx = {pointer, data...}; a lone pointer byte is a valid write of
         * zero data bytes (sets the device read pointer). */
        st = HAL_I2C_Mem_Write(m_hi2c, static_cast<uint16_t>(addr7) << 1, tx[0],
                               I2C_MEMADD_SIZE_8BIT,
                               const_cast<uint8_t*>(tx + 1),
                               static_cast<uint16_t>(tx_len - 1), timeout_ms);
    }
    if (st == HAL_OK) {
        return true;
    }
    if (st == HAL_TIMEOUT) {
        ++m_timeouts;
    } else {
        ++m_errors;
    }
    /* A failed transaction can leave the peripheral busy; drop the sticky
     * error flags so the next poll starts clean. */
    __HAL_I2C_CLEAR_FLAG(m_hi2c, I2C_FLAG_AF);
    return false;
}

bool HalI2cBus::isReady(uint8_t addr7, uint32_t timeout_ms) {
    const HAL_StatusTypeDef st =
        HAL_I2C_IsDeviceReady(m_hi2c, static_cast<uint16_t>(addr7) << 1,
                              1U /* trials */, timeout_ms);
    if (st == HAL_TIMEOUT) {
        ++m_timeouts;
    }
    return st == HAL_OK;
}

} // namespace Inverter
