#include "Inverter/Drivers/CAN/CanProtocolTransport.h"

#include "Inverter/Telemetry.h"

#include "main.h"

#include <cstring>

namespace Inverter {

namespace {
constexpr uint8_t FLAG_START = 0x80;
constexpr uint8_t FLAG_END   = 0x40;
constexpr uint8_t SEQ_MASK   = 0x0F;
constexpr uint32_t RX_TIMEOUT_MS = 500;

/* Big packet buffers live in AXI SRAM: DTCM is for the control path. */
uint8_t s_tx_buf[CanProtocolTransport::MAX_PACKET]
    __attribute__((section(".dma_buffers")));
uint8_t s_rx_buf[CanProtocolTransport::MAX_PACKET]
    __attribute__((section(".dma_buffers")));
}

bool CanProtocolTransport::init(uint8_t bus, uint32_t id_base,
                                PacketHandler handler, void* user) {
    if (bus >= CanBus::NUM_BUSES || id_base > 0x7FEU || handler == nullptr) {
        return false;
    }
    m_bus = bus;
    m_id_from_host = id_base;
    m_id_to_host = id_base + 1;
    m_handler = handler;
    m_user = user;
    m_tx_seq = 0;
    m_tx_len = 0;
    m_tx_off = 0;
    m_tx_started = false;
    m_rx_expect = 0;
    m_rx_have = 0;
    m_rx_seq = 0;
    m_rx_last_ms = 0;
    m_rx_dropped = 0;
    return true;
}

bool CanProtocolTransport::sendPacket(const uint8_t* packet, size_t len) {
    if (packet == nullptr || len == 0 || len > MAX_PACKET) {
        return false;
    }
    if (m_tx_len != 0) {
        /* One packet in flight at a time; the caller's decimation policy
         * decides what is worth the bus. */
        return false;
    }
    std::memcpy(s_tx_buf, packet, len);
    m_tx_len = len;
    m_tx_off = 0;
    m_tx_started = false;
    return true;
}

void CanProtocolTransport::pumpTx() {
    /* Drip-feed the in-flight packet into the bus TX ring with slack for
     * other traffic (graph nodes, session replies).  A few chunks per
     * main-loop iteration when the ring has room; the ring never
     * overflows, so no packet ever loses a chunk to contention. */
    for (int chunks = 0; chunks < 4 && m_tx_len != 0; ++chunks) {
        if (canBus().txFree(m_bus) < 2) {
            return;
        }

        uint8_t f[8];
        if (!m_tx_started) {
            f[0] = FLAG_START | (m_tx_seq & SEQ_MASK);
            f[1] = static_cast<uint8_t>(m_tx_len & 0xFF);
            f[2] = static_cast<uint8_t>((m_tx_len >> 8) & 0xFF);
            const size_t first = m_tx_len < 5 ? m_tx_len : 5;
            std::memcpy(f + 3, s_tx_buf, first);
            for (size_t i = 3 + first; i < 8; ++i) {
                f[i] = 0;
            }
            if (first >= m_tx_len) {
                f[0] |= FLAG_END;
            }
            if (!canBus().send(m_bus, m_id_to_host, false, f, 8)) {
                return;
            }
            m_tx_started = true;
            ++m_tx_seq;
            m_tx_off = first;
        } else {
            const size_t remaining = m_tx_len - m_tx_off;
            const size_t n = remaining < 7 ? remaining : 7;
            uint8_t c[8] = {};
            c[0] = (m_tx_seq & SEQ_MASK);
            std::memcpy(c + 1, s_tx_buf + m_tx_off, n);
            if (m_tx_off + n >= m_tx_len) {
                c[0] |= FLAG_END;
            }
            if (!canBus().send(m_bus, m_id_to_host, false, c, 8)) {
                return;
            }
            ++m_tx_seq;
            m_tx_off += n;
        }

        if (m_tx_off >= m_tx_len) {
            m_tx_len = 0;
        }
    }
}

void CanProtocolTransport::onFrame(const CanBus::Frame& f) {
    if (f.id != m_id_from_host || f.ext || f.dlc < 1 || f.dlc > 8) {
        return;
    }
    const uint8_t flags = f.data[0] & (FLAG_START | FLAG_END);
    const uint8_t seq = f.data[0] & SEQ_MASK;

    if (flags & FLAG_START) {
        if (m_rx_expect != 0) {
            ++m_rx_dropped;
        }
        if (f.dlc < 3) {
            ++m_rx_dropped;
            return;
        }
        m_rx_expect = static_cast<size_t>(f.data[1]) |
                      (static_cast<size_t>(f.data[2]) << 8);
        if (m_rx_expect == 0 || m_rx_expect > MAX_PACKET) {
            m_rx_expect = 0;
            ++m_rx_dropped;
            return;
        }
        const size_t available = f.dlc - 3;
        m_rx_have = available < m_rx_expect ? available : m_rx_expect;
        std::memcpy(s_rx_buf, f.data + 3, m_rx_have);
        m_rx_seq = (seq + 1) & SEQ_MASK;
        m_rx_last_ms = HAL_GetTick();
    } else {
        if (m_rx_expect == 0 || seq != m_rx_seq) {
            ++m_rx_dropped;
            m_rx_expect = 0;  /* resync on next START */
            return;
        }
        const size_t available = f.dlc - 1;
        const size_t remaining = m_rx_expect - m_rx_have;
        const size_t n = available < remaining ? available : remaining;
        std::memcpy(s_rx_buf + m_rx_have, f.data + 1, n);
        m_rx_have += n;
        m_rx_seq = (seq + 1) & SEQ_MASK;
        m_rx_last_ms = HAL_GetTick();
    }

    const bool ended = (flags & FLAG_END) != 0;
    if (ended && m_rx_expect != 0 && m_rx_have != m_rx_expect) {
        /* END before the declared packet length is malformed. */
        m_rx_expect = 0;
        ++m_rx_dropped;
        return;
    }
    if (!ended && m_rx_expect != 0 && m_rx_have == m_rx_expect) {
        /* The sender supplied the declared length without END.  Drop now
         * instead of retaining a packet that can only overflow or time out. */
        m_rx_expect = 0;
        ++m_rx_dropped;
        return;
    }
    if (ended && m_rx_expect != 0) {
        const size_t len = m_rx_expect;
        m_rx_expect = 0;
        if (m_handler != nullptr) {
            m_handler(s_rx_buf, len, m_user);
        }
    }
}

void CanProtocolTransport::update() {
    pumpTx();
    if (m_rx_expect != 0 &&
        (HAL_GetTick() - m_rx_last_ms) > RX_TIMEOUT_MS) {
        m_rx_expect = 0;
        ++m_rx_dropped;
    }
}

} // namespace Inverter
