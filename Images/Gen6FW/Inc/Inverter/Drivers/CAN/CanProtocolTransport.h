#pragma once

#include <cstddef>
#include <cstdint>

#include "Inverter/Drivers/CAN/CanBus.h"

namespace Inverter {

/**
 * @brief InverterProtocol packets over classic CAN frames (segmentation).
 *
 * Frame format (8-byte classic frames):
 *   byte 0: bit7 = start, bit6 = end, bits[3:0] = chunk sequence (mod 16)
 *   start frame: bytes 1..2 = total packet length (u16 LE), bytes 3..7 = data (5)
 *   cont. frame: bytes 1..7 = data (7)
 *
 * Packets (16-byte IVP header + payload + CRC) are segmented on TX and
 * reassembled on RX with sequence checking and a 500 ms inter-chunk
 * timeout.  Two CAN IDs (KV Can.Proto.IdBase, default 0x700):
 * base+0 = host->device, base+1 = device->host.
 */
class CanProtocolTransport {
public:
    static constexpr size_t MAX_PACKET = 700;

    typedef void (*PacketHandler)(const uint8_t* packet, size_t len, void* user);

    bool init(uint8_t bus, uint32_t id_base, PacketHandler handler, void* user);

    /** @brief Segment and queue a complete packet.  Never blocks. */
    bool sendPacket(const uint8_t* packet, size_t len);

    /** @brief Feed a received CAN frame (from the CanBus sniffer hook). */
    void onFrame(const CanBus::Frame& f);

    /** @brief Main-loop housekeeping: paced TX pump + reassembly timeout. */
    void update();

    uint32_t rxDropped() const { return m_rx_dropped; }
    bool txBusy() const { return m_tx_len != 0; }

private:
    void pumpTx();
    uint8_t  m_bus = 1;
    uint32_t m_id_to_host = 0x701;
    uint32_t m_id_from_host = 0x700;
    PacketHandler m_handler = nullptr;
    void*    m_user = nullptr;

    uint8_t  m_tx_seq = 0;

    /* In-flight TX packet state (the buffers themselves are file-static in
     * the .cpp, in AXI SRAM). */
    size_t   m_tx_len = 0;
    size_t   m_tx_off = 0;
    bool     m_tx_started = false;

    size_t   m_rx_expect = 0;
    size_t   m_rx_have = 0;
    uint8_t  m_rx_seq = 0;
    uint32_t m_rx_last_ms = 0;
    uint32_t m_rx_dropped = 0;
};

} // namespace Inverter
