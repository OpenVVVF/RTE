/*
 * sil_can.cpp — SIL stubs for the CAN subsystem:
 *   Src/Inverter/Drivers/CAN/CanBus.cpp    (bus driver — no hardware bus here)
 *   Src/Inverter/Drivers/CAN/FdcanFault.cpp (error-IRQ wiring)
 *
 * CanSession / CanProtocolTransport / TraceRecorder compile verbatim on top
 * of this CanBus API.  Behavior: KV-gated enables are honored (Can.A.En
 * default 0, Can.B.En default 1), TX always "succeeds" and is dropped, RX
 * mailboxes stay empty (no host traffic in SIL).
 */
#include "Inverter/Drivers/CAN/CanBus.h"
#include "Inverter/Drivers/CAN/FdcanFault.h"
#include "Inverter/Drivers/Storage/RteParamStore.h"
#include "Inverter/Telemetry.h"

namespace Inverter {

static CanBus s_can;

CanBus& canBus() {
    return s_can;
}

namespace {
float kvOr(const char* key, float dflt) {
    float v = dflt;
    if (RteParamStore::isReady()) {
        (void)RteParamStore::get(key, &v);
    }
    return v;
}
} // namespace

bool CanBus::init() {
    m_enabled[0] = kvOr("Can.A.En", 0.0f) >= 0.5f;
    m_enabled[1] = kvOr("Can.B.En", 1.0f) >= 0.5f;
    m_bitrate = static_cast<uint32_t>(kvOr("Can.BitRate", 500000.0f));
    m_fd_enabled[0] = false;
    m_fd_enabled[1] = kvOr("Can.Trace.En", 0.0f) >= 0.5f;
    resetState();
    return true;
}

void CanBus::update() {}

bool CanBus::send(uint8_t bus, uint32_t, bool, const uint8_t*, uint8_t) {
    if (bus >= NUM_BUSES || !m_enabled[bus]) return false;
    ++m_tx_frames[bus];
    return true;
}

bool CanBus::sendFd(uint8_t bus, uint32_t, const uint8_t*, uint8_t) {
    if (bus >= NUM_BUSES || !m_enabled[bus]) return false;
    ++m_fd_tx_frames[bus];
    return true;
}

bool CanBus::rxLatest(uint8_t, uint32_t, bool, Frame&, uint32_t* seqOut) {
    if (seqOut != nullptr) *seqOut = 0;
    return false;
}

size_t CanBus::txFree(uint8_t) const { return TX_RING; }

void CanBus::setRxHook(RxHook hook, void* user) {
    m_hook = hook;
    m_hook_user = user;
}

bool CanBus::enabled(uint8_t bus) const {
    return bus < NUM_BUSES && m_enabled[bus];
}

bool CanBus::fdEnabled(uint8_t bus) const {
    return bus < NUM_BUSES && m_fd_enabled[bus];
}

void CanBus::printStatus(uint8_t bus) const {
    if (bus >= NUM_BUSES) return;
    Telemetry::printf("[CAN] bus %u (SIL): enabled=%d fd=%d tx=%lu rx=%lu",
                      static_cast<unsigned>(bus + 1),
                      m_enabled[bus] ? 1 : 0, m_fd_enabled[bus] ? 1 : 0,
                      static_cast<unsigned long>(m_tx_frames[bus]),
                      static_cast<unsigned long>(m_rx_frames[bus]));
}

void CanBus::printRecentRx(uint8_t) const {
    Telemetry::printf("[CAN] (SIL) no bus traffic");
}

void CanBus::onRxFifo0(FDCAN_HandleTypeDef*) {}
void CanBus::serviceTx(FDCAN_HandleTypeDef*) {}

FDCAN_HandleTypeDef* CanBus::handle(uint8_t bus) const {
    (void)bus;
    return nullptr;
}

bool CanBus::validId(uint32_t id, bool ext) {
    return ext ? (id <= 0x1FFFFFFFU) : (id <= 0x7FFU);
}

void CanBus::resetState() {}

bool CanBus::applyTiming(uint8_t, uint32_t, uint32_t) { return true; }
void CanBus::kickTx(uint8_t) {}
void CanBus::recoverIfBusOff(uint8_t) {}
void CanBus::processRx(uint8_t) {}
void CanBus::storeRx(uint8_t, const Frame&) {}

bool fdcanFaultInit() {
    return true;
}

} // namespace Inverter
