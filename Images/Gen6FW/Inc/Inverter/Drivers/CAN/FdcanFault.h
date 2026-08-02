#pragma once

namespace Inverter {

/**
 * @brief Enable FDCAN error-status notifications and route them to FaultManager.
 *
 * Arms notifications only for buses enabled via KV (Can.A.En/Can.B.En); a
 * disabled bus can never latch a CAN fault.  Call from CanBus::init() once
 * the enables are known.
 */
bool fdcanFaultInit(bool enable_bus_a, bool enable_bus_b);

} // namespace Inverter
