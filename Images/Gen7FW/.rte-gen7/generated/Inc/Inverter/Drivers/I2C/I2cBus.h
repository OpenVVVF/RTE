#pragma once

#include <cstdint>

namespace Inverter {

/**
 * @brief Abstract I2C master port — the single funnel for all I2C driver I/O.
 *
 * Every Gen7 I2C driver (on-board temp sensor, rail monitor) talks to the bus
 * exclusively through this interface, so the drivers and their register
 * decode/encode math can be compiled and unit-tested on the host against a
 * mock bus (see tests/host/), with no HAL dependency.
 *
 * Transaction model matches the register-pointer devices we drive
 * (TMP102-family, INA226/INA228/INA3221):
 *   transfer(addr, tx={reg[, data...]}, rx=n)  ==
 *     START, addr+W, tx bytes, [repeated START, addr+R, rx bytes], STOP
 * A null/empty @p rx reads nothing (pure register write); @p tx always starts
 * with the register pointer byte.
 *
 * All calls are blocking-with-timeout and must only be made from main-loop /
 * shell context — never from an ISR.
 */
class II2cBus {
public:
    virtual ~II2cBus() = default;

    /**
     * @brief Register-pointer transaction (see class comment).
     * @param addr7      7-bit slave address (0x08..0x77).
     * @param tx         Bytes to write; tx[0] is the register pointer.
     * @param tx_len     Number of bytes in @p tx (>= 1).
     * @param rx         Receive buffer; may be nullptr when rx_len == 0.
     * @param rx_len     Bytes to read after a repeated START (0 = write only).
     * @param timeout_ms Per-phase bus timeout.
     * @return true on success; false on NACK/bus error/timeout.
     */
    virtual bool transfer(uint8_t addr7,
                          const uint8_t* tx, uint8_t tx_len,
                          uint8_t* rx, uint8_t rx_len,
                          uint32_t timeout_ms) = 0;

    /**
     * @brief Address-only presence probe (HAL_I2C_IsDeviceReady equivalent).
     * @return true if the slave ACKed its address.
     */
    virtual bool isReady(uint8_t addr7, uint32_t timeout_ms) = 0;
};

} // namespace Inverter
