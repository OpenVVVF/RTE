#include <gtest/gtest.h>

#include "inverter_protocol/trace_protocol.h"

#include <cstring>

TEST(TraceProtocol, DataRoundTripUsesExactlyOneCanFdPayload) {
    ivp_trace_data_frame_t source{};
    source.capture_id = 9;
    source.first_sample_sequence = 1001;
    source.first_sample_cycles = 0x12345678;
    source.delta_cycles_div8[0] = 13750;
    source.delta_cycles_div8[1] = 13749;
    for (std::size_t s = 0; s < IVP_TRACE_SAMPLES_PER_DATA_FRAME; ++s)
        for (std::size_t c = 0; c < IVP_TRACE_CHANNELS; ++c)
            source.samples[s][c] = static_cast<int16_t>(s * 100 - c * 7);

    uint8_t payload[IVP_TRACE_PAYLOAD_SIZE]{};
    ASSERT_TRUE(ivp_trace_encode_data(&source, payload));
    EXPECT_EQ(ivp_trace_frame_type(payload), IVP_TRACE_FRAME_DATA);
    ivp_trace_data_frame_t decoded{};
    ASSERT_TRUE(ivp_trace_decode_data(payload, &decoded));
    EXPECT_EQ(decoded.capture_id, source.capture_id);
    EXPECT_EQ(decoded.first_sample_sequence, source.first_sample_sequence);
    EXPECT_EQ(decoded.first_sample_cycles, source.first_sample_cycles);
    EXPECT_EQ(decoded.delta_cycles_div8[0], source.delta_cycles_div8[0]);
    EXPECT_EQ(decoded.delta_cycles_div8[1], source.delta_cycles_div8[1]);
    EXPECT_EQ(std::memcmp(decoded.samples, source.samples, sizeof(source.samples)), 0);
}

TEST(TraceProtocol, SchemaAndSparseEventRoundTrip) {
    ivp_trace_schema_frame_t schema{};
    schema.capture_id = 3;
    schema.channel = 7;
    schema.scale = 0.125f;
    std::strcpy(schema.name, "phase_current_a");
    uint8_t payload[IVP_TRACE_PAYLOAD_SIZE]{};
    ASSERT_TRUE(ivp_trace_encode_schema(&schema, payload));
    ivp_trace_schema_frame_t decoded_schema{};
    ASSERT_TRUE(ivp_trace_decode_schema(payload, &decoded_schema));
    EXPECT_STREQ(decoded_schema.name, schema.name);
    EXPECT_FLOAT_EQ(decoded_schema.scale, schema.scale);

    ivp_trace_event_frame_t event{3, 31, true, 99, 4, 18.5f};
    ASSERT_TRUE(ivp_trace_encode_event(&event, payload));
    ivp_trace_event_frame_t decoded_event{};
    ASSERT_TRUE(ivp_trace_decode_event(payload, &decoded_event));
    EXPECT_TRUE(decoded_event.snapshot);
    EXPECT_EQ(decoded_event.sample_sequence, 99u);
    EXPECT_FLOAT_EQ(decoded_event.value, 18.5f);
}

TEST(TraceProtocol, RejectsWrongMagicVersionAndChannel) {
    uint8_t payload[IVP_TRACE_PAYLOAD_SIZE]{};
    ivp_trace_data_frame_t data{};
    EXPECT_FALSE(ivp_trace_decode_data(payload, &data));
    payload[0] = IVP_TRACE_MAGIC;
    payload[1] = IVP_TRACE_VERSION + 1;
    payload[2] = IVP_TRACE_FRAME_DATA;
    EXPECT_FALSE(ivp_trace_decode_data(payload, &data));

    ivp_trace_schema_frame_t schema{};
    schema.channel = IVP_TRACE_MAX_CHANNELS;
    EXPECT_FALSE(ivp_trace_encode_schema(&schema, payload));
}

TEST(TraceProtocol, SchemaNameBoundaryAt31Chars) {
    /* 31 chars + NUL fills the 32-byte wire slot exactly. */
    ivp_trace_schema_frame_t schema{};
    schema.capture_id = 1;
    schema.channel = 2;
    schema.scale = 1.0f;
    const std::string max_name(IVP_TRACE_SCHEMA_NAME_SIZE - 1, 'n');
    std::strcpy(schema.name, max_name.c_str());

    uint8_t payload[IVP_TRACE_PAYLOAD_SIZE]{};
    ASSERT_TRUE(ivp_trace_encode_schema(&schema, payload));
    ivp_trace_schema_frame_t decoded{};
    ASSERT_TRUE(ivp_trace_decode_schema(payload, &decoded));
    EXPECT_STREQ(decoded.name, max_name.c_str());
}

TEST(TraceProtocol, SchemaEncodeRejectsUnterminatedName) {
    /* A name filling all 32 bytes with no NUL would lose its last character
     * to the decoder's forced termination; the encoder refuses it. */
    ivp_trace_schema_frame_t schema{};
    schema.channel = 0;
    std::memset(schema.name, 'x', IVP_TRACE_SCHEMA_NAME_SIZE);

    uint8_t payload[IVP_TRACE_PAYLOAD_SIZE]{};
    EXPECT_FALSE(ivp_trace_encode_schema(&schema, payload));
}

TEST(TraceProtocol, SchemaDecodeTruncatesFullUnterminatedName) {
    /* Defense in depth: even a hand-crafted full 32-byte name on the wire
     * decodes as 31 chars + NUL (documented truncation). */
    uint8_t payload[IVP_TRACE_PAYLOAD_SIZE]{};
    payload[0] = IVP_TRACE_MAGIC;
    payload[1] = IVP_TRACE_VERSION;
    payload[2] = IVP_TRACE_FRAME_SCHEMA;
    std::memset(payload + 12, 'y', IVP_TRACE_SCHEMA_NAME_SIZE);

    ivp_trace_schema_frame_t decoded{};
    ASSERT_TRUE(ivp_trace_decode_schema(payload, &decoded));
    std::string expected(IVP_TRACE_SCHEMA_NAME_SIZE - 1, 'y');
    EXPECT_STREQ(decoded.name, expected.c_str());
}
