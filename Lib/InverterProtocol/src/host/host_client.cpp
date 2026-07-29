#include "inverter_protocol/host/host_client.h"

#include "inverter_protocol/packet_builder.h"
#include "inverter_protocol/packet_parser.h"

#include <chrono>
#include <cstring>

namespace ivp {

InverterClient::InverterClient() = default;

InverterClient::~InverterClient() {
    stop();
}

bool InverterClient::start(const std::string& port, int baud) {
    stop();
    running_.store(true);
    thread_ = std::thread(&InverterClient::threadMain, this, port, baud);
    return true;
}

void InverterClient::stop() {
    running_.store(false);
    if (thread_.joinable()) {
        thread_.join();
    }
    transport_.close();
}

bool InverterClient::isRunning() const {
    return running_.load();
}

bool InverterClient::sendCommandLine(const std::string& line) {
    return transport_.sendLine(line);
}

bool InverterClient::sendCommand(uint8_t opcode, uint8_t req_id) {
    uint8_t payload[16];
    ivp_command_req_builder_t b;
    if (ivp_command_req_begin(&b, payload, sizeof(payload), opcode, req_id) != IVP_OK)
        return false;

    uint8_t packet[64];
    size_t packet_len = 0;
    if (ivp_packet_encode(IVP_MSG_COMMAND_REQ, 0, 0, payload, static_cast<uint16_t>(b.len),
                          packet, sizeof(packet), &packet_len) != IVP_OK)
        return false;

    return transport_.sendPacket(packet, packet_len);
}

bool InverterClient::sendCommand(uint8_t opcode, uint8_t req_id, float a) {
    uint8_t payload[32];
    ivp_command_req_builder_t b;
    if (ivp_command_req_begin(&b, payload, sizeof(payload), opcode, req_id) != IVP_OK)
        return false;
    if (ivp_command_req_add_f32(&b, a) != IVP_OK) return false;

    uint8_t packet[64];
    size_t packet_len = 0;
    if (ivp_packet_encode(IVP_MSG_COMMAND_REQ, 0, 0, payload, static_cast<uint16_t>(b.len),
                          packet, sizeof(packet), &packet_len) != IVP_OK)
        return false;

    return transport_.sendPacket(packet, packet_len);
}

bool InverterClient::sendCommand(uint8_t opcode, uint8_t req_id, float a, float b) {
    uint8_t payload[32];
    ivp_command_req_builder_t builder;
    if (ivp_command_req_begin(&builder, payload, sizeof(payload), opcode, req_id) != IVP_OK)
        return false;
    if (ivp_command_req_add_f32(&builder, a) != IVP_OK) return false;
    if (ivp_command_req_add_f32(&builder, b) != IVP_OK) return false;

    uint8_t packet[64];
    size_t packet_len = 0;
    if (ivp_packet_encode(IVP_MSG_COMMAND_REQ, 0, 0, payload, static_cast<uint16_t>(builder.len),
                          packet, sizeof(packet), &packet_len) != IVP_OK)
        return false;

    return transport_.sendPacket(packet, packet_len);
}

ClientStats InverterClient::stats() const {
    std::lock_guard<std::mutex> lk(stats_mtx_);
    return stats_;
}

void InverterClient::threadMain(const std::string& port, int baud) {
    auto reopen = [&](bool first) -> bool {
        transport_.close();
        std::this_thread::sleep_for(std::chrono::milliseconds(first ? 200 : 150));

        while (running_.load()) {
            if (transport_.open(port, baud)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (!running_.load()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return true;
    };

    if (!reopen(true)) return;

    auto t0 = std::chrono::steady_clock::now();
    auto last_good = t0;
    uint64_t frames_in_window = 0;
    uint64_t bytes_in_window = 0;
    auto window_start = t0;

    while (running_.load()) {
        auto now = std::chrono::steady_clock::now();
        if (now - last_good > std::chrono::seconds(2)) {
            if (!reopen(false)) break;
            last_good = std::chrono::steady_clock::now();
        }

        uint8_t packet[UartTransport::RX_FRAME_CAP];
        int n = transport_.receivePacket(packet, sizeof(packet));
        if (n < 0) {
            std::lock_guard<std::mutex> lk(stats_mtx_);
            ++stats_.bad_frames;
            continue;
        }
        if (n == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        bytes_in_window += static_cast<uint64_t>(n);
        stats_.rx_bytes += static_cast<uint64_t>(n);

        ivp_header_t h;
        const uint8_t* payload = nullptr;
        uint16_t payload_len = 0;
        ivp_result_t r = ivp_packet_parse(packet, static_cast<size_t>(n), &h, &payload, &payload_len);

        if (r != IVP_OK) {
            std::lock_guard<std::mutex> lk(stats_mtx_);
            ++stats_.bad_frames;
            if (r == IVP_ERR_BAD_CRC) ++stats_.reject_crc;
            else if (r == IVP_ERR_BAD_MAGIC || r == IVP_ERR_BAD_VERSION || r == IVP_ERR_BAD_MSG_TYPE)
                ++stats_.reject_hdr;
            else if (r == IVP_ERR_BAD_LENGTH)
                ++stats_.reject_len;
            continue;
        }

        if (h.msg_type == IVP_MSG_TELEMETRY_DEFINE) {
            ivp_define_iter_t it;
            if (ivp_telemetry_define_iter_init(payload, payload_len, &it) == IVP_OK) {
                std::lock_guard<std::mutex> lk(reg_mtx_);
                uint16_t id;
                uint8_t type;
                const char* key;
                uint8_t key_len;
                while (ivp_telemetry_define_iter_next(&it, &id, &type, &key, &key_len)) {
                    KeyDef def;
                    def.type = type;
                    def.key.assign(key, key_len);
                    registry_[id] = std::move(def);
                }
            }
        } else if (h.msg_type == IVP_MSG_TELEMETRY_DATA) {
            handleTelemetryData(payload, payload_len, h.time_us);
        } else if (h.msg_type == IVP_MSG_COMMAND_RSP) {
            handleCommandResponse(payload, payload_len);
        }

        {
            std::lock_guard<std::mutex> lk(stats_mtx_);
            ++stats_.good_frames;
            stats_.last_seq = h.seq;
        }
        last_good = std::chrono::steady_clock::now();

        ++frames_in_window;
        float dt = std::chrono::duration<float>(now - window_start).count();
        if (dt >= 1.0f) {
            float hz = frames_in_window / dt;
            float bps = bytes_in_window / dt;
            {
                std::lock_guard<std::mutex> lk(stats_mtx_);
                stats_.rx_hz = hz;
                stats_.rx_bytes_per_sec = bps;
            }
            if (cb_stats_) cb_stats_(stats_);
            frames_in_window = 0;
            bytes_in_window = 0;
            window_start = now;
        }
    }
}

void InverterClient::handleTelemetryData(const uint8_t* payload, uint16_t payload_len, uint32_t time_us) {
    ivp_data_iter_t it;
    if (ivp_telemetry_data_iter_init(payload, payload_len, &it) != IVP_OK) return;

    ivp_data_item_t item;
    while (ivp_telemetry_data_iter_next(&it, &item)) {
        std::string key;
        {
            std::lock_guard<std::mutex> lk(reg_mtx_);
            auto it_reg = registry_.find(item.id);
            if (it_reg == registry_.end()) continue;
            key = it_reg->second.key;
        }

        if (item.type == IVP_VT_F32 && cb_f32_) {
            cb_f32_(item.id, key, item.v.f32, time_us);
        } else if (item.type == IVP_VT_STR && cb_str_) {
            std::string value(item.v.str.data, item.v.str.len);
            if (key == "print" && cb_console_) {
                cb_console_(value);
            } else {
                cb_str_(item.id, key, value, time_us);
            }
        } else if (item.type == IVP_VT_STR_FRAG && cb_str_) {
            std::string value(item.v.frag.data, item.v.frag.len);
            if (key == "print" && cb_console_) {
                cb_console_(value);
            } else {
                cb_str_(item.id, key, value, time_us);
            }
        }
    }
}

void InverterClient::handleCommandResponse(const uint8_t* payload, uint16_t payload_len) {
    if (!cb_rsp_) return;

    uint8_t req_id, status;
    ivp_arg_iter_t args;
    if (ivp_command_rsp_parse(payload, payload_len, &req_id, &status, &args) != IVP_OK)
        return;

    std::vector<float> fargs;
    ivp_arg_t arg;
    while (ivp_arg_iter_next(&args, &arg)) {
        if (arg.type == IVP_ARG_F32) fargs.push_back(arg.v.f32);
        else if (arg.type == IVP_ARG_U8)  fargs.push_back(static_cast<float>(arg.v.u8));
        else if (arg.type == IVP_ARG_U16) fargs.push_back(static_cast<float>(arg.v.u16));
        else if (arg.type == IVP_ARG_U32) fargs.push_back(static_cast<float>(arg.v.u32));
    }

    cb_rsp_(req_id, status, fargs);
}

} // namespace ivp
