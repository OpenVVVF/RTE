#pragma once

#include <cstddef>
#include <cstdint>

#include "Inverter/Drivers/CAN/CanProtocolTransport.h"

namespace Inverter {

/**
 * @brief CAN session/capability layer: attach negotiation, capability
 * grants, telemetry routing, command ingress.
 *
 * Nothing streams and no commands are accepted from CAN until a host
 * attaches (HELLO -> ATTACH_RSP with device info + KV allow mask), requests
 * capabilities (CAP_REQ -> CAP_RSP with granted mask), and keeps the
 * session alive with HEARTBEAT (Can.Proto.SessTimeoutMs, default 3000 ms;
 * timeout drops the session and stops streaming — fail-safe on
 * host/cable loss).
 *
 * KV policy (RteParamStore):
 *   Can.Proto.En        master enable (default 0)
 *   Can.Proto.Bus       bus number 1=A / 2=B (default 2)
 *   Can.Proto.IdBase    CAN ID base (default 0x700; +0 host->dev, +1 dev->host)
 *   Can.Proto.AllowTelem allow TELEMETRY grants (default 1)
 *   Can.Proto.AllowCmd   allow COMMANDS grants (default 1)
 *   Can.Proto.AllowFlash allow FLASH grants (default 0; reserved)
 *   Can.Proto.SessTimeoutMs heartbeat timeout (default 3000)
 *
 * Auth hook: every grant decision funnels through grantAllowed(); the
 * future password/token check plugs in there (AUTH_REQ/RSP message IDs are
 * reserved in the shared protocol header).
 */
class CanSession {
public:
    bool init();
    void update();

    bool attached() const { return m_attached; }
    uint8_t grants() const { return m_grants; }

    /** @brief Telemetry sink entry: stream a raw packet to the session. */
    bool sendTelemetryPacket(const uint8_t* packet, size_t len);

    /** @brief CanBus sniffer hook entry (static trampolines in the .cpp). */
    void onCanFrame(uint8_t bus, const CanBus::Frame& f);
    void onPacket(const uint8_t* packet, size_t len);

private:
    void detach(const char* why);
    void applyTelemetryRouting();
    bool grantAllowed(uint8_t cap_bit) const;
    bool sendPacket(uint8_t msg_type, const uint8_t* payload, size_t payload_len);
    void sendAttachRsp();
    void handleCapReq(const uint8_t* payload, size_t len);
    void handleCommandReq(const uint8_t* packet, size_t len);

    CanProtocolTransport m_transport;
    bool     m_enabled = false;
    bool     m_attached = false;
    uint8_t  m_grants = 0;
    uint8_t  m_allows = 0;
    uint8_t  m_bus = 2;
    uint32_t m_last_hb_ms = 0;
    uint32_t m_timeout_ms = 3000;
    uint32_t m_pkt_seq = 0;
    uint8_t  m_telem_div = 5;
    uint32_t m_telem_count = 0;
};

/** @brief Global CAN session instance. */
CanSession& canSession();

} // namespace Inverter
