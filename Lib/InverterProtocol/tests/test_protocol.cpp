#include <gtest/gtest.h>

#include "inverter_protocol/packet_builder.h"
#include "inverter_protocol/packet_parser.h"
#include "inverter_protocol/protocol.h"

#include <cstring>
#include <vector>

/* ========================================================================
 * CRC tests
 * ======================================================================== */
TEST(Crc, EmptyInput) {
    EXPECT_EQ(ivp_crc16_ccitt(nullptr, 0), 0xFFFFu);
}

TEST(Crc, KnownVector) {
    const uint8_t data[] = "123456789";
    EXPECT_EQ(ivp_crc16_ccitt(data, sizeof(data) - 1), 0x29B1u);
}

TEST(Crc, ConsistentWithPacket) {
    uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t packet[64];
    size_t len = 0;
    ASSERT_EQ(ivp_packet_encode(IVP_MSG_TELEMETRY_DATA, 7, 1000,
                                payload, sizeof(payload),
                                packet, sizeof(packet), &len),
              IVP_OK);

    uint16_t calc = ivp_crc16_ccitt(packet, len - 2);
    uint16_t stored = ivp_read_u16le(packet + len - 2);
    EXPECT_EQ(calc, stored);
}

/* ========================================================================
 * COBS tests
 * ======================================================================== */
TEST(Cobs, RoundTripWithZeros) {
    const uint8_t in[] = {0x00, 0x00, 0x00};
    uint8_t enc[16];
    size_t enc_len = ivp_cobs_encode(in, sizeof(in), enc, sizeof(enc));
    ASSERT_GT(enc_len, 0u);

    uint8_t dec[16];
    size_t dec_len = ivp_cobs_decode(enc, enc_len, dec, sizeof(dec));
    ASSERT_EQ(dec_len, sizeof(in));
    EXPECT_EQ(std::memcmp(dec, in, sizeof(in)), 0);
}

TEST(Cobs, RoundTripNoZeros) {
    const uint8_t in[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    uint8_t enc[16];
    size_t enc_len = ivp_cobs_encode(in, sizeof(in), enc, sizeof(enc));
    ASSERT_GT(enc_len, 0u);

    uint8_t dec[16];
    size_t dec_len = ivp_cobs_decode(enc, enc_len, dec, sizeof(dec));
    ASSERT_EQ(dec_len, sizeof(in));
    EXPECT_EQ(std::memcmp(dec, in, sizeof(in)), 0);
}

TEST(Cobs, RoundTripMixed) {
    const uint8_t in[] = {0x11, 0x00, 0x00, 0x22, 0x33, 0x00, 0x44};
    uint8_t enc[32];
    size_t enc_len = ivp_cobs_encode(in, sizeof(in), enc, sizeof(enc));
    ASSERT_GT(enc_len, 0u);

    uint8_t dec[32];
    size_t dec_len = ivp_cobs_decode(enc, enc_len, dec, sizeof(dec));
    ASSERT_EQ(dec_len, sizeof(in));
    EXPECT_EQ(std::memcmp(dec, in, sizeof(in)), 0);
}

TEST(Cobs, DecodeInvalidZeroCode) {
    const uint8_t enc[] = {0x00};
    uint8_t dec[8];
    EXPECT_EQ(ivp_cobs_decode(enc, sizeof(enc), dec, sizeof(dec)), 0u);
}

/* ========================================================================
 * Header / packet parse tests
 * ======================================================================== */
TEST(PacketParse, RoundTrip) {
    uint8_t payload[] = {0xAB, 0xCD};
    uint8_t packet[64];
    size_t len = 0;
    ASSERT_EQ(ivp_packet_encode(IVP_MSG_TELEMETRY_DEFINE, 42, 123456,
                                payload, sizeof(payload),
                                packet, sizeof(packet), &len),
              IVP_OK);

    ivp_header_t h;
    const uint8_t* out_payload = nullptr;
    uint16_t out_payload_len = 0;
    ASSERT_EQ(ivp_packet_parse(packet, len, &h, &out_payload, &out_payload_len), IVP_OK);

    EXPECT_EQ(h.magic, IVP_MAGIC);
    EXPECT_EQ(h.version, IVP_VERSION);
    EXPECT_EQ(h.msg_type, IVP_MSG_TELEMETRY_DEFINE);
    EXPECT_EQ(h.payload_len, sizeof(payload));
    EXPECT_EQ(h.seq, 42u);
    EXPECT_EQ(h.time_us, 123456u);
    ASSERT_EQ(out_payload_len, sizeof(payload));
    EXPECT_EQ(std::memcmp(out_payload, payload, sizeof(payload)), 0);
}

TEST(PacketParse, BadCrc) {
    uint8_t payload[] = {0x01};
    uint8_t packet[64];
    size_t len = 0;
    ASSERT_EQ(ivp_packet_encode(IVP_MSG_TELEMETRY_DATA, 1, 0,
                                payload, sizeof(payload),
                                packet, sizeof(packet), &len),
              IVP_OK);

    /* Corrupt a byte in the payload. */
    packet[IVP_HEADER_SIZE] ^= 0xFF;

    ivp_header_t h;
    const uint8_t* out_payload = nullptr;
    uint16_t out_payload_len = 0;
    EXPECT_EQ(ivp_packet_parse(packet, len, &h, &out_payload, &out_payload_len), IVP_ERR_BAD_CRC);
}

TEST(PacketParse, BadMagic) {
    uint8_t payload[] = {0x01};
    uint8_t packet[64];
    size_t len = 0;
    ASSERT_EQ(ivp_packet_encode(IVP_MSG_TELEMETRY_DATA, 1, 0,
                                payload, sizeof(payload),
                                packet, sizeof(packet), &len),
              IVP_OK);

    packet[0] = 0xFF;

    ivp_header_t h;
    const uint8_t* out_payload = nullptr;
    uint16_t out_payload_len = 0;
    EXPECT_EQ(ivp_packet_parse(packet, len, &h, &out_payload, &out_payload_len), IVP_ERR_BAD_MAGIC);
}

/* ========================================================================
 * Telemetry DEFINE payload tests
 * ======================================================================== */
TEST(DefinePayload, RoundTrip) {
    uint8_t payload[128];
    ivp_define_builder_t b;
    ASSERT_EQ(ivp_telemetry_define_begin(&b, payload, sizeof(payload)), IVP_OK);
    ASSERT_EQ(ivp_telemetry_define_add_f32(&b, 0x0001, "v_bus", 5), IVP_OK);
    ASSERT_EQ(ivp_telemetry_define_add_f32(&b, 0x0002, "i_u", 3), IVP_OK);
    ASSERT_EQ(ivp_telemetry_define_add_str(&b, 0x8001, "print", 5), IVP_OK);

    ivp_define_iter_t it;
    ASSERT_EQ(ivp_telemetry_define_iter_init(payload, static_cast<uint16_t>(b.len), &it), IVP_OK);

    uint16_t id;
    uint8_t type;
    const char* key;
    uint8_t key_len;

    ASSERT_TRUE(ivp_telemetry_define_iter_next(&it, &id, &type, &key, &key_len));
    EXPECT_EQ(id, 0x0001);
    EXPECT_EQ(type, IVP_VT_F32);
    EXPECT_EQ(std::string(key, key_len), "v_bus");

    ASSERT_TRUE(ivp_telemetry_define_iter_next(&it, &id, &type, &key, &key_len));
    EXPECT_EQ(id, 0x0002);
    EXPECT_EQ(type, IVP_VT_F32);
    EXPECT_EQ(std::string(key, key_len), "i_u");

    ASSERT_TRUE(ivp_telemetry_define_iter_next(&it, &id, &type, &key, &key_len));
    EXPECT_EQ(id, 0x8001);
    EXPECT_EQ(type, IVP_VT_STR);
    EXPECT_EQ(std::string(key, key_len), "print");

    EXPECT_FALSE(ivp_telemetry_define_iter_next(&it, &id, &type, &key, &key_len));
}

/* ========================================================================
 * Telemetry DATA payload tests
 * ======================================================================== */
TEST(DataPayload, RoundTripF32AndStr) {
    uint8_t payload[128];
    ivp_data_builder_t b;
    ASSERT_EQ(ivp_telemetry_data_begin(&b, payload, sizeof(payload)), IVP_OK);
    ASSERT_EQ(ivp_telemetry_data_add_f32(&b, 0x0001, 48.0f), IVP_OK);
    ASSERT_EQ(ivp_telemetry_data_add_str(&b, 0x8001, "hello", 5), IVP_OK);

    ivp_data_iter_t it;
    ASSERT_EQ(ivp_telemetry_data_iter_init(payload, static_cast<uint16_t>(b.len), &it), IVP_OK);

    ivp_data_item_t item;
    ASSERT_TRUE(ivp_telemetry_data_iter_next(&it, &item));
    EXPECT_EQ(item.id, 0x0001);
    EXPECT_EQ(item.type, IVP_VT_F32);
    EXPECT_FLOAT_EQ(item.v.f32, 48.0f);

    ASSERT_TRUE(ivp_telemetry_data_iter_next(&it, &item));
    EXPECT_EQ(item.id, 0x8001);
    EXPECT_EQ(item.type, IVP_VT_STR);
    EXPECT_EQ(std::string(item.v.str.data, item.v.str.len), "hello");

    EXPECT_FALSE(ivp_telemetry_data_iter_next(&it, &item));
}

TEST(DataPayload, RoundTripFragment) {
    uint8_t payload[128];
    ivp_data_builder_t b;
    ASSERT_EQ(ivp_telemetry_data_begin(&b, payload, sizeof(payload)), IVP_OK);
    ASSERT_EQ(ivp_telemetry_data_add_str_frag(&b, 0x8001, IVP_SF_START, "abc", 3), IVP_OK);
    ASSERT_EQ(ivp_telemetry_data_add_str_frag(&b, 0x8001, IVP_SF_END, "def", 3), IVP_OK);

    ivp_data_iter_t it;
    ASSERT_EQ(ivp_telemetry_data_iter_init(payload, static_cast<uint16_t>(b.len), &it), IVP_OK);

    ivp_data_item_t item;
    ASSERT_TRUE(ivp_telemetry_data_iter_next(&it, &item));
    EXPECT_EQ(item.type, IVP_VT_STR_FRAG);
    EXPECT_EQ(item.v.frag.frag, IVP_SF_START);
    EXPECT_EQ(std::string(item.v.frag.data, item.v.frag.len), "abc");

    ASSERT_TRUE(ivp_telemetry_data_iter_next(&it, &item));
    EXPECT_EQ(item.type, IVP_VT_STR_FRAG);
    EXPECT_EQ(item.v.frag.frag, IVP_SF_END);
    EXPECT_EQ(std::string(item.v.frag.data, item.v.frag.len), "def");
}

/* ========================================================================
 * Command payload tests
 * ======================================================================== */
TEST(CommandPayload, RequestRoundTrip) {
    uint8_t payload[64];
    ivp_command_req_builder_t b;
    ASSERT_EQ(ivp_command_req_begin(&b, payload, sizeof(payload), 0x10, 0x05), IVP_OK);
    ASSERT_EQ(ivp_command_req_add_u8(&b, 0x01), IVP_OK);
    ASSERT_EQ(ivp_command_req_add_f32(&b, 3.14f), IVP_OK);

    uint8_t opcode, req_id;
    ivp_arg_iter_t args;
    ASSERT_EQ(ivp_command_req_parse(payload, static_cast<uint16_t>(b.len),
                                    &opcode, &req_id, &args),
              IVP_OK);
    EXPECT_EQ(opcode, 0x10);
    EXPECT_EQ(req_id, 0x05);

    ivp_arg_t arg;
    ASSERT_TRUE(ivp_arg_iter_next(&args, &arg));
    EXPECT_EQ(arg.type, IVP_ARG_U8);
    EXPECT_EQ(arg.v.u8, 0x01);

    ASSERT_TRUE(ivp_arg_iter_next(&args, &arg));
    EXPECT_EQ(arg.type, IVP_ARG_F32);
    EXPECT_FLOAT_EQ(arg.v.f32, 3.14f);

    EXPECT_FALSE(ivp_arg_iter_next(&args, &arg));
}

TEST(CommandPayload, ResponseRoundTrip) {
    uint8_t payload[64];
    ivp_command_rsp_builder_t b;
    ASSERT_EQ(ivp_command_rsp_begin(&b, payload, sizeof(payload), 0x05, 0x00), IVP_OK);
    ASSERT_EQ(ivp_command_rsp_add_u16(&b, 1234), IVP_OK);
    ASSERT_EQ(ivp_command_rsp_add_str(&b, "ok", 2), IVP_OK);

    uint8_t req_id, status;
    ivp_arg_iter_t results;
    ASSERT_EQ(ivp_command_rsp_parse(payload, static_cast<uint16_t>(b.len),
                                    &req_id, &status, &results),
              IVP_OK);
    EXPECT_EQ(req_id, 0x05);
    EXPECT_EQ(status, 0x00);

    ivp_arg_t arg;
    ASSERT_TRUE(ivp_arg_iter_next(&results, &arg));
    EXPECT_EQ(arg.type, IVP_ARG_U16);
    EXPECT_EQ(arg.v.u16, 1234u);

    ASSERT_TRUE(ivp_arg_iter_next(&results, &arg));
    EXPECT_EQ(arg.type, IVP_ARG_STR);
    EXPECT_EQ(std::string(arg.v.str.data, arg.v.str.len), "ok");

    EXPECT_FALSE(ivp_arg_iter_next(&results, &arg));
}

/* ========================================================================
 * End-to-end frame encode/decode tests
 * ======================================================================== */
TEST(EndToEnd, DefineFrameCobsUartStyle) {
    uint8_t payload[128];
    ivp_define_builder_t b;
    ASSERT_EQ(ivp_telemetry_define_begin(&b, payload, sizeof(payload)), IVP_OK);
    ASSERT_EQ(ivp_telemetry_define_add_f32(&b, 0x1234, "temp", 4), IVP_OK);

    uint8_t packet[256];
    size_t packet_len = 0;
    ASSERT_EQ(ivp_packet_encode(IVP_MSG_TELEMETRY_DEFINE, 1, 5000,
                                payload, static_cast<uint16_t>(b.len),
                                packet, sizeof(packet), &packet_len),
              IVP_OK);

    /* UART-style COBS + delimiter. */
    uint8_t encoded[512];
    size_t enc_len = ivp_cobs_encode(packet, packet_len, encoded, sizeof(encoded));
    ASSERT_GT(enc_len, 0u);
    encoded[enc_len] = 0;

    uint8_t decoded[512];
    size_t dec_len = ivp_cobs_decode(encoded, enc_len, decoded, sizeof(decoded));
    ASSERT_EQ(dec_len, packet_len);
    EXPECT_EQ(std::memcmp(decoded, packet, packet_len), 0);

    ivp_header_t h;
    const uint8_t* out_payload = nullptr;
    uint16_t out_payload_len = 0;
    ASSERT_EQ(ivp_packet_parse(decoded, dec_len, &h, &out_payload, &out_payload_len), IVP_OK);

    ivp_define_iter_t it;
    ASSERT_EQ(ivp_telemetry_define_iter_init(out_payload, out_payload_len, &it), IVP_OK);
    uint16_t id;
    uint8_t type;
    const char* key;
    uint8_t key_len;
    ASSERT_TRUE(ivp_telemetry_define_iter_next(&it, &id, &type, &key, &key_len));
    EXPECT_EQ(id, 0x1234);
    EXPECT_EQ(type, IVP_VT_F32);
    EXPECT_EQ(std::string(key, key_len), "temp");
}

/* ========================================================================
 * UartTransport receive-path tests (POSIX pty based)
 * ======================================================================== */
#ifndef _WIN32

#include "inverter_protocol/host/uart_transport.h"

#include <fcntl.h>
#include <pty.h>
#include <unistd.h>

namespace {

struct Pty {
    int master = -1;
    char slaveName[128] = {};

    ~Pty() {
        if (master >= 0) ::close(master);
    }
};

// Opens a pty pair; the slave side is closed immediately since UartTransport
// reopens it by path.
bool OpenPty(Pty& pty) {
    int slave = -1;
    if (openpty(&pty.master, &slave, pty.slaveName, nullptr, nullptr) != 0) {
        return false;
    }
    ::close(slave);
    return true;
}

std::vector<uint8_t> MakePacket(uint32_t seq) {
    uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t packet[64];
    size_t len = 0;
    EXPECT_EQ(ivp_packet_encode(IVP_MSG_TELEMETRY_DATA, seq, 1000,
                                payload, sizeof(payload),
                                packet, sizeof(packet), &len),
              IVP_OK);
    return std::vector<uint8_t>(packet, packet + len);
}

void AppendFramed(std::vector<uint8_t>& stream, const std::vector<uint8_t>& packet) {
    uint8_t encoded[128];
    const size_t encLen = ivp_cobs_encode(packet.data(), packet.size(),
                                          encoded, sizeof(encoded));
    stream.insert(stream.end(), encoded, encoded + encLen);
    stream.push_back(0x00);
}

void WriteAll(int fd, const std::vector<uint8_t>& data) {
    size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        ASSERT_GT(n, 0);
        off += static_cast<size_t>(n);
    }
}

}  // namespace

// Regression test: one read() chunk at 100 Hz typically holds several frames.
// Bytes past the first delimiter must stay buffered, not dropped.
TEST(UartTransport, MultipleFramesInOneReadChunk) {
    Pty pty;
    ASSERT_TRUE(OpenPty(pty));

    ivp::UartTransport transport;
    ASSERT_TRUE(transport.open(pty.slaveName));

    const auto p1 = MakePacket(1);
    const auto p2 = MakePacket(2);
    const auto p3 = MakePacket(3);

    std::vector<uint8_t> stream;
    AppendFramed(stream, p1);
    AppendFramed(stream, p2);
    AppendFramed(stream, p3);
    WriteAll(pty.master, stream);

    uint8_t out[ivp::UartTransport::RX_FRAME_CAP];
    for (const auto& expected : {p1, p2, p3}) {
        const int n = transport.receivePacket(out, sizeof(out));
        ASSERT_EQ(n, static_cast<int>(expected.size()));
        EXPECT_EQ(std::memcmp(out, expected.data(), expected.size()), 0);
    }
}

// A garbage frame must not poison the frames that follow it.
TEST(UartTransport, GarbageFrameResyncs) {
    Pty pty;
    ASSERT_TRUE(OpenPty(pty));

    ivp::UartTransport transport;
    ASSERT_TRUE(transport.open(pty.slaveName));

    const auto p1 = MakePacket(7);
    std::vector<uint8_t> stream = {0x41, 0x42, 0x00};  // invalid COBS frame
    AppendFramed(stream, p1);
    WriteAll(pty.master, stream);

    uint8_t out[ivp::UartTransport::RX_FRAME_CAP];
    EXPECT_EQ(transport.receivePacket(out, sizeof(out)), -1);

    const int n = transport.receivePacket(out, sizeof(out));
    ASSERT_EQ(n, static_cast<int>(p1.size()));
    EXPECT_EQ(std::memcmp(out, p1.data(), p1.size()), 0);
}

// A frame split across two writes must complete once the tail arrives.
TEST(UartTransport, SplitFrameCompletes) {
    Pty pty;
    ASSERT_TRUE(OpenPty(pty));

    ivp::UartTransport transport;
    ASSERT_TRUE(transport.open(pty.slaveName));

    const auto p1 = MakePacket(9);
    std::vector<uint8_t> stream;
    AppendFramed(stream, p1);

    const size_t half = stream.size() / 2;
    WriteAll(pty.master, std::vector<uint8_t>(stream.begin(), stream.begin() + half));

    uint8_t out[ivp::UartTransport::RX_FRAME_CAP];
    EXPECT_EQ(transport.receivePacket(out, sizeof(out)), 0);

    WriteAll(pty.master, std::vector<uint8_t>(stream.begin() + half, stream.end()));
    const int n = transport.receivePacket(out, sizeof(out));
    ASSERT_EQ(n, static_cast<int>(p1.size()));
    EXPECT_EQ(std::memcmp(out, p1.data(), p1.size()), 0);
}

#endif  // _WIN32

/* ========================================================================
 * Session/capability message type tests (protocol v1, msg types 7-14)
 * ======================================================================== */
TEST(SessionMessages, NewTypesAccepted) {
    /* Every msg type in the session range must pass header validation. */
    for (uint8_t type = IVP_MSG_HELLO; type <= IVP_MSG_AUTH_RSP; ++type) {
        uint8_t packet[64];
        size_t len = 0;
        ASSERT_EQ(ivp_packet_encode(type, 1, 0, nullptr, 0,
                                    packet, sizeof(packet), &len),
                  IVP_OK);

        ivp_header_t h;
        const uint8_t* payload = nullptr;
        uint16_t payload_len = 0;
        ASSERT_EQ(ivp_packet_parse(packet, len, &h, &payload, &payload_len), IVP_OK)
            << "msg_type " << static_cast<int>(type) << " rejected";
        EXPECT_EQ(h.msg_type, type);
        EXPECT_EQ(h.payload_len, 0u);
    }
}

TEST(SessionMessages, OutOfRangeTypeRejected) {
    uint8_t packet[64];
    size_t len = 0;
    ASSERT_EQ(ivp_packet_encode(IVP_MSG_HEARTBEAT, 1, 0, nullptr, 0,
                                packet, sizeof(packet), &len),
              IVP_OK);

    /* One past the last defined type must still be rejected. */
    packet[5] = IVP_MSG_AUTH_RSP + 1;  /* msg_type field offset in header */

    ivp_header_t h;
    const uint8_t* payload = nullptr;
    uint16_t payload_len = 0;
    EXPECT_EQ(ivp_packet_parse(packet, len, &h, &payload, &payload_len),
              IVP_ERR_BAD_MSG_TYPE);
}
