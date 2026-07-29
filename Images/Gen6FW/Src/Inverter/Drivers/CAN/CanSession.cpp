#include "Inverter/Drivers/CAN/CanSession.h"

#include "Inverter/Command/CommandManager.h"
#include "Inverter/Drivers/Storage/RteParamStore.h"
#include "Inverter/Telemetry.h"

#include <inverter_protocol/protocol.h>
#include <inverter_protocol/packet_builder.h>
#include <inverter_protocol/packet_parser.h>

#include "main.h"

#include <cstring>

namespace Inverter {

namespace {

CanSession s_instance;

float kvOr(const char* key, float def) {
    float v = def;
    if (RteParamStore::isReady()) {
        RteParamStore::get(key, &v);
    }
    return v;
}

/* Static trampolines (C-style hooks into the singleton). */
void snifferTrampoline(uint8_t bus, const CanBus::Frame& f, void*) {
    s_instance.onCanFrame(bus, f);
}

void packetTrampoline(const uint8_t* packet, size_t len, void*) {
    s_instance.onPacket(packet, len);
}

bool telemetrySinkTrampoline(const uint8_t* packet, size_t len) {
    return canSession().sendTelemetryPacket(packet, len);
}

} // namespace

CanSession& canSession() {
    return s_instance;
}

bool CanSession::sendTelemetryPacket(const uint8_t* packet, size_t len) {
    if (!m_attached || (m_grants & IVP_CAP_TELEMETRY) == 0) {
        return false;
    }
    /* Decimate: full-rate telemetry (~600 B packets at 100 Hz = ~9k CAN
     * frames/s) exceeds classic-CAN capacity; forward 1-in-N whole packets
     * (KV Can.Proto.TelemDiv, default 5). */
    if (m_telem_div > 1 && (++m_telem_count % m_telem_div) != 0) {
        return false;
    }
    return m_transport.sendPacket(packet, len);
}

bool CanSession::init() {
    m_enabled = kvOr("Can.Proto.En", 0.0f) != 0.0f;
    if (!m_enabled) {
        return true;
    }
    m_bus = static_cast<uint8_t>(kvOr("Can.Proto.Bus", 2.0f));
    const uint32_t id_base = static_cast<uint32_t>(kvOr("Can.Proto.IdBase", 1792.0f));  /* 0x700 */
    m_timeout_ms = static_cast<uint32_t>(kvOr("Can.Proto.SessTimeoutMs", 3000.0f));
    m_telem_div = static_cast<uint8_t>(kvOr("Can.Proto.TelemDiv", 5.0f));
    if (m_telem_div == 0) m_telem_div = 1;
    m_allows = (kvOr("Can.Proto.AllowTelem", 1.0f) != 0.0f ? IVP_CAP_TELEMETRY : 0) |
               (kvOr("Can.Proto.AllowCmd", 1.0f) != 0.0f ? IVP_CAP_COMMANDS : 0) |
               (kvOr("Can.Proto.AllowFlash", 0.0f) != 0.0f ? IVP_CAP_FLASH : 0);

    if (!canBus().enabled(m_bus - 1)) {
        Telemetry::printf("[CAN] session: bus %u disabled; session off",
                          static_cast<unsigned>(m_bus));
        m_enabled = false;
        return false;
    }

    if (!m_transport.init(m_bus - 1, id_base, packetTrampoline, nullptr)) {
        return false;
    }
    canBus().setRxHook(snifferTrampoline, nullptr);
    Telemetry::printf("[CAN] session: listening on bus %u id 0x%03lX (timeout %lu ms)",
                      static_cast<unsigned>(m_bus),
                      static_cast<unsigned long>(id_base),
                      static_cast<unsigned long>(m_timeout_ms));
    return true;
}

bool CanSession::grantAllowed(uint8_t cap_bit) const {
    /* Auth hook: the future password/token check plugs in here.  v1: KV
     * allow mask is the whole policy. */
    return (m_allows & cap_bit) != 0;
}

void CanSession::applyTelemetryRouting() {
    if (m_attached && (m_grants & IVP_CAP_TELEMETRY) != 0) {
        Telemetry::set_extra_frame_sink(telemetrySinkTrampoline);
    } else {
        Telemetry::set_extra_frame_sink(nullptr);
    }
}

void CanSession::detach(const char* why) {
    if (!m_attached) {
        return;
    }
    m_attached = false;
    m_grants = 0;
    applyTelemetryRouting();
    Telemetry::printf("[CAN] session detached (%s)", why);
}

bool CanSession::sendPacket(uint8_t msg_type, const uint8_t* payload,
                            size_t payload_len) {
    uint8_t buf[16 + 64 + 2];
    if (payload_len > 64) {
        return false;
    }
    size_t len = 0;
    if (ivp_packet_encode(msg_type, m_pkt_seq++, 0,
                          static_cast<const uint8_t*>(payload),
                          static_cast<uint16_t>(payload_len),
                          buf, sizeof(buf), &len) != IVP_OK) {
        return false;
    }
    return m_transport.sendPacket(buf, len);
}

void CanSession::sendAttachRsp() {
    /* Payload: allow mask + device name (len-prefixed). */
    static constexpr char NAME[] = "gen6fw";
    uint8_t payload[2 + sizeof(NAME)] = {};
    payload[0] = m_allows;
    payload[1] = sizeof(NAME) - 1;
    std::memcpy(payload + 2, NAME, sizeof(NAME) - 1);
    sendPacket(IVP_MSG_ATTACH_RSP, payload, 2 + sizeof(NAME) - 1);
}

void CanSession::handleCapReq(const uint8_t* payload, size_t len) {
    if (len < 1) {
        return;
    }
    const uint8_t req = payload[0];
    uint8_t granted = 0;
    if ((req & IVP_CAP_TELEMETRY) && grantAllowed(IVP_CAP_TELEMETRY)) {
        granted |= IVP_CAP_TELEMETRY;
    }
    if ((req & IVP_CAP_COMMANDS) && grantAllowed(IVP_CAP_COMMANDS)) {
        granted |= IVP_CAP_COMMANDS;
    }
    if ((req & IVP_CAP_FLASH) && grantAllowed(IVP_CAP_FLASH)) {
        granted |= IVP_CAP_FLASH;
    }
    m_grants = granted;
    applyTelemetryRouting();
    sendPacket(IVP_MSG_CAP_RSP, &granted, 1);
    Telemetry::printf("[CAN] session caps granted: 0x%02X", granted);
}

void CanSession::handleCommandReq(const uint8_t* packet, size_t len) {
    if ((m_grants & IVP_CAP_COMMANDS) == 0) {
        sendPacket(IVP_MSG_NACK, nullptr, 0);
        return;
    }
    /* Parse header, then the command args: first STR arg is the line. */
    ivp_header_t hdr;
    if (ivp_header_decode(packet, &hdr) != IVP_OK ||
        len < IVP_HEADER_SIZE + 2) {
        return;
    }
    const uint8_t* payload = packet + IVP_HEADER_SIZE;
    const uint16_t payload_len = hdr.payload_len;

    uint8_t opcode = 0, req_id = 0;
    ivp_arg_iter_t args;
    if (ivp_command_req_parse(payload, payload_len, &opcode, &req_id,
                              &args) != IVP_OK) {
        sendPacket(IVP_MSG_NACK, nullptr, 0);
        return;
    }
    (void)opcode;

    const char* line = nullptr;
    size_t line_len = 0;
    ivp_arg_t arg;
    while (ivp_arg_iter_next(&args, &arg)) {
        if (arg.type == IVP_ARG_STR) {
            line = arg.v.str.data;
            line_len = arg.v.str.len;
            break;
        }
    }
    if (line == nullptr) {
        sendPacket(IVP_MSG_NACK, nullptr, 0);
        return;
    }

    char cmd[96];
    const size_t n = line_len < sizeof(cmd) - 1 ? line_len : sizeof(cmd) - 1;
    std::memcpy(cmd, line, n);
    cmd[n] = '\0';

    /* Output flows back via the telemetry stream (grant TELEMETRY to see
     * it); the RSP is just an ack carrying the request id. */
    CommandManager::instance().processLine(cmd);
    sendPacket(IVP_MSG_COMMAND_RSP, &req_id, 1);
}

void CanSession::onPacket(const uint8_t* packet, size_t len) {
    if (len < IVP_HEADER_SIZE + 2) {
        return;
    }
    ivp_header_t hdr;
    if (ivp_header_decode(packet, &hdr) != IVP_OK) {
        return;
    }
    /* CRC16 over header+payload must match the trailing 2 bytes. */
    if (ivp_crc16_ccitt(packet, len - 2) != ivp_read_u16le(packet + len - 2)) {
        return;  /* CRC mismatch: drop silently */
    }

    switch (hdr.msg_type) {
        case IVP_MSG_HELLO:
            m_attached = true;
            m_grants = 0;
            m_last_hb_ms = HAL_GetTick();
            sendAttachRsp();
            Telemetry::printf("[CAN] host attached");
            break;
        case IVP_MSG_CAP_REQ:
            if (m_attached) {
                handleCapReq(packet + IVP_HEADER_SIZE, hdr.payload_len);
            }
            break;
        case IVP_MSG_HEARTBEAT:
            if (m_attached) {
                m_last_hb_ms = HAL_GetTick();
            }
            break;
        case IVP_MSG_DETACH:
            detach("host requested");
            break;
        case IVP_MSG_COMMAND_REQ:
            if (m_attached) {
                handleCommandReq(packet, len);
            }
            break;
        default:
            break;
    }
}

void CanSession::onCanFrame(uint8_t bus, const CanBus::Frame& f) {
    if (!m_enabled || bus != m_bus - 1) {
        return;
    }
    m_transport.onFrame(f);
}

void CanSession::update() {
    if (!m_enabled) {
        return;
    }
    m_transport.update();
    if (m_attached &&
        (HAL_GetTick() - m_last_hb_ms) > m_timeout_ms) {
        detach("heartbeat timeout");
    }
}

} // namespace Inverter
