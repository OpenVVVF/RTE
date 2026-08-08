#include <RTEAutomation/ProcessRunner.h>
#include <inverter_protocol/trace_protocol.h>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifndef RTE_CLI_PATH
#error RTE_CLI_PATH must name the rte executable
#endif

TEST(RteCli, ReportsStructuredVersion) {
    RTEAutomation::ProcessSpec spec;
    spec.executable = RTE_CLI_PATH;
    spec.arguments = {"--format", "jsonl", "--version"};
    std::vector<std::string> lines;
    const auto result = RTEAutomation::RunProcess(
        spec, [&](const std::string& line) { lines.push_back(line); });
    ASSERT_TRUE(result.started);
    ASSERT_EQ(result.exitCode, 0);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_NE(lines[0].find("\"event\":\"version\""), std::string::npos);
}

namespace {
void WriteU32(std::ostream& out, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out.put(static_cast<char>(value >> (8U * i)));
}
void WriteU64(std::ostream& out, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) out.put(static_cast<char>(value >> (8U * i)));
}
void WriteCaptureRecord(std::ostream& out, std::uint64_t hostNs, std::uint32_t id,
                        const std::uint8_t payload[IVP_TRACE_PAYLOAD_SIZE]) {
    WriteU64(out, hostNs);
    WriteU32(out, id);
    out.put(static_cast<char>(IVP_TRACE_PAYLOAD_SIZE));
    out.write("\0\0\0", 3);
    out.write(reinterpret_cast<const char*>(payload), IVP_TRACE_PAYLOAD_SIZE);
}
}

TEST(RteCli, ExportsFastSamplesAndLastValueHeldSparseEvents) {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto capture = std::filesystem::temp_directory_path() /
        ("rte_cli_trace_" + std::to_string(suffix) + ".rtecap");
    const auto csv = capture.string() + ".csv";
    std::ofstream file(capture, std::ios::binary);
    file.write("RTECAP1\0", 8);
    std::uint8_t payload[IVP_TRACE_PAYLOAD_SIZE]{};
    ivp_trace_schema_frame_t fastSchema{};
    fastSchema.capture_id = 1;
    fastSchema.channel = 0;
    fastSchema.scale = 0.5f;
    std::strcpy(fastSchema.name, "fast0");
    ASSERT_TRUE(ivp_trace_encode_schema(&fastSchema, payload));
    WriteCaptureRecord(file, 1, 0x681, payload);
    ivp_trace_schema_frame_t sparseSchema{};
    sparseSchema.capture_id = 1;
    sparseSchema.channel = 8;
    sparseSchema.scale = 1.0f;
    std::strcpy(sparseSchema.name, "setpoint");
    ASSERT_TRUE(ivp_trace_encode_schema(&sparseSchema, payload));
    WriteCaptureRecord(file, 2, 0x681, payload);
    ivp_trace_event_frame_t event{1, 8, true, 10, 0, 42.5f};
    ASSERT_TRUE(ivp_trace_encode_event(&event, payload));
    WriteCaptureRecord(file, 3, 0x682, payload);
    ivp_trace_data_frame_t data{};
    data.capture_id = 1;
    data.first_sample_sequence = 9;
    data.samples[1][0] = 4;
    ASSERT_TRUE(ivp_trace_encode_data(&data, payload));
    WriteCaptureRecord(file, 4, 0x680, payload);
    file.close();

    RTEAutomation::ProcessSpec spec;
    spec.executable = RTE_CLI_PATH;
    spec.arguments = {"trace", "export", "--input", capture.string(),
                      "--output", csv};
    const auto result = RTEAutomation::RunProcess(spec, [](const std::string&) {});
    ASSERT_TRUE(result.started);
    ASSERT_EQ(result.exitCode, 0);
    std::ifstream exported(csv);
    std::ostringstream contents;
    contents << exported.rdbuf();
    EXPECT_NE(contents.str().find("fast0"), std::string::npos);
    EXPECT_NE(contents.str().find("setpoint"), std::string::npos);
    EXPECT_NE(contents.str().find("42.5"), std::string::npos);

    std::error_code ignored;
    std::filesystem::remove(capture, ignored);
    std::filesystem::remove(csv, ignored);
}
