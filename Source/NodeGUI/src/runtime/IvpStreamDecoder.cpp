#include "IvpStreamDecoder.h"

#include <inverter_protocol/packet_parser.h>

#include <algorithm>
#include <cstdio>

namespace NodeGUI::runtime {

void IvpStreamDecoder::FeedBytes(const uint8_t* data, size_t n) {
    if (!data || n == 0) {
        return;
    }

    // Rolling-window byte count for the RX-rate stats; wire bytes, not
    // decoded payload bytes, so the bandwidth estimate reflects the link.
    bytesInWindow_ += static_cast<uint64_t>(n);
    stats_.rx_bytes += static_cast<uint64_t>(n);

    for (size_t i = 0; i < n; ++i) {
        const uint8_t b = data[i];
        if (skipUntilDelimiter_) {
            // Discard the remainder of an oversize frame; the delimiter after
            // it ends the frame instead of opening a parsed one.
            if (b == 0x00) {
                skipUntilDelimiter_ = false;
            }
            continue;
        }
        if (b != 0x00) {
            if (frameLen_ < kMaxEncodedFrame) {
                frameBuf_[frameLen_++] = b;
            } else {
                // Oversize frame: drop the buffered prefix and skip the rest.
                frameLen_ = 0;
                skipUntilDelimiter_ = true;
            }
            continue;
        }

        if (frameLen_ == 0) {
            // Back-to-back delimiters carry no frame.
            continue;
        }
        HandleFrame(frameBuf_, frameLen_);
        frameLen_ = 0;
    }
}

void IvpStreamDecoder::Reset() {
    frameLen_ = 0;
    skipUntilDelimiter_ = false;
    framesInWindow_ = 0;
    bytesInWindow_ = 0;
    registry_.clear();
    partialStrings_.clear();
}

void IvpStreamDecoder::EmitStats(double dt_seconds) {
    if (dt_seconds <= 0.0) {
        return;
    }
    stats_.rx_hz = static_cast<float>(framesInWindow_ / dt_seconds);
    stats_.rx_bytes_per_sec = static_cast<float>(bytesInWindow_ / dt_seconds);
    framesInWindow_ = 0;
    bytesInWindow_ = 0;
    if (onStats) {
        onStats(stats_);
    }
}

void IvpStreamDecoder::HandleFrame(const uint8_t* encoded, size_t len) {
    uint8_t packet[kMaxEncodedFrame];
    const size_t packet_len = ivp_cobs_decode(encoded, len, packet, sizeof(packet));
    if (packet_len == 0 || packet_len < IVP_HEADER_SIZE + 2u) {
        ++stats_.bad_frames;
        ++stats_.reject_decode;
        return;
    }

    ivp_header_t header{};
    const uint8_t* payload = nullptr;
    uint16_t payload_len = 0;
    const ivp_result_t result =
        ivp_packet_parse(packet, packet_len, &header, &payload, &payload_len);
    if (result != IVP_OK) {
        ++stats_.bad_frames;
        switch (result) {
        case IVP_ERR_BAD_CRC:
            ++stats_.reject_crc;
            break;
        case IVP_ERR_BAD_MAGIC:
        case IVP_ERR_BAD_VERSION:
        case IVP_ERR_BAD_MSG_TYPE:
            ++stats_.reject_hdr;
            break;
        case IVP_ERR_BAD_LENGTH:
            ++stats_.reject_len;
            break;
        default:
            ++stats_.reject_decode;
            break;
        }
        return;
    }

    ++stats_.good_frames;
    stats_.last_seq = header.seq;
    ++framesInWindow_;

    switch (header.msg_type) {
    case IVP_MSG_TELEMETRY_DEFINE:
        HandleDefine(payload, payload_len);
        break;
    case IVP_MSG_TELEMETRY_DATA:
        HandleData(payload, payload_len, header.time_us);
        break;
    case IVP_MSG_COMMAND_RSP:
        HandleCommandResponse(payload, payload_len);
        break;
    default:
        break;
    }
}

void IvpStreamDecoder::HandleDefine(const uint8_t* payload, uint16_t payload_len) {
    ivp_define_iter_t it;
    if (ivp_telemetry_define_iter_init(payload, payload_len, &it) != IVP_OK) {
        ++stats_.reject_decode;
        return;
    }

    uint16_t id = 0;
    uint8_t type = 0;
    const char* key = nullptr;
    uint8_t key_len = 0;
    while (ivp_telemetry_define_iter_next(&it, &id, &type, &key, &key_len)) {
        KeyDef def;
        def.type = type;
        def.key.assign(key, key + key_len);
        registry_[id] = std::move(def);
    }
}

void IvpStreamDecoder::HandleData(const uint8_t* payload,
                                  uint16_t payload_len,
                                  uint32_t time_us) {
    ivp_data_iter_t it;
    if (ivp_telemetry_data_iter_init(payload, payload_len, &it) != IVP_OK) {
        ++stats_.reject_decode;
        return;
    }

    ivp_data_item_t item{};
    while (ivp_telemetry_data_iter_next(&it, &item)) {
        const auto reg = registry_.find(item.id);
        if (reg == registry_.end()) {
            // DATA arrived before (or without) the matching DEFINE.
            ++stats_.reject_decode;
            continue;
        }
        const std::string& key = reg->second.key;

        if (item.type == IVP_VT_F32) {
            if (onF32Value) {
                onF32Value(item.id, key, item.v.f32, time_us);
            }
        } else if (item.type == IVP_VT_STR) {
            const std::string value(item.v.str.data, item.v.str.len);
            partialStrings_.erase(key);
            IngestString(item.id, key, value, time_us);
        } else if (item.type == IVP_VT_STR_FRAG) {
            auto& part = partialStrings_[key];
            if (item.v.frag.frag & IVP_SF_START) {
                part.buf.clear();
            }
            part.buf.append(item.v.frag.data, item.v.frag.len);
            part.lastTimeUs = time_us;
            if (item.v.frag.frag & IVP_SF_END) {
                IngestString(item.id, key, part.buf, time_us);
                partialStrings_.erase(key);
            }
        }
    }

    // Discard string fragments that stopped arriving mid-message (>2 s of
    // device time without a new fragment).
    for (auto pit = partialStrings_.begin(); pit != partialStrings_.end();) {
        if (time_us - pit->second.lastTimeUs > 2000000u) {
            pit = partialStrings_.erase(pit);
        } else {
            ++pit;
        }
    }
}

void IvpStreamDecoder::HandleCommandResponse(const uint8_t* payload, uint16_t payload_len) {
    uint8_t req_id = 0;
    uint8_t status = 0;
    ivp_arg_iter_t args;
    if (ivp_command_rsp_parse(payload, payload_len, &req_id, &status, &args) != IVP_OK) {
        ++stats_.reject_decode;
        return;
    }
    if (onConsoleLine) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "[rsp #%u status %u]",
                      static_cast<unsigned>(req_id), static_cast<unsigned>(status));
        onConsoleLine(buf);
    }
}

void IvpStreamDecoder::IngestString(uint16_t id,
                                    const std::string& key,
                                    const std::string& value,
                                    uint32_t time_us) {
    // Same convention as ivp::InverterClient: "print" is console text.
    if (key == "print") {
        if (onConsoleLine) {
            onConsoleLine(value);
        }
    } else if (onStringValue) {
        onStringValue(id, key, value, time_us);
    }
}

}  // namespace NodeGUI::runtime
