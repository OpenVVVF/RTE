#include "TraceCli.h"

#include "inverter_protocol/trace_protocol.h"

#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <string>

#ifdef __linux__
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {
namespace fs = std::filesystem;
constexpr std::array<char, 8> kMagic{'R','T','E','C','A','P','1','\0'};

struct CaptureRecord {
    std::uint64_t hostNs = 0;
    std::uint32_t canId = 0;
    std::uint8_t length = 0;
    std::array<std::uint8_t, IVP_TRACE_PAYLOAD_SIZE> payload{};
};

void WriteU32(std::ostream& out, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out.put(static_cast<char>(value >> (i * 8U)));
}
void WriteU64(std::ostream& out, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) out.put(static_cast<char>(value >> (i * 8U)));
}
bool ReadU32(std::istream& in, std::uint32_t& value) {
    value = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const int c = in.get(); if (c == EOF) return false;
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << (i * 8U);
    }
    return true;
}
bool ReadU64(std::istream& in, std::uint64_t& value) {
    value = 0;
    for (unsigned i = 0; i < 8; ++i) {
        const int c = in.get(); if (c == EOF) return false;
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(c)) << (i * 8U);
    }
    return true;
}

bool WriteRecord(std::ostream& out, const CaptureRecord& record) {
    WriteU64(out, record.hostNs);
    WriteU32(out, record.canId);
    out.put(static_cast<char>(record.length));
    out.write("\0\0\0", 3);
    out.write(reinterpret_cast<const char*>(record.payload.data()), record.payload.size());
    return static_cast<bool>(out);
}

bool ReadRecord(std::istream& in, CaptureRecord& record) {
    if (!ReadU64(in, record.hostNs)) return false;
    if (!ReadU32(in, record.canId)) return false;
    const int length = in.get();
    if (length == EOF) return false;
    record.length = static_cast<std::uint8_t>(length);
    in.ignore(3);
    in.read(reinterpret_cast<char*>(record.payload.data()), record.payload.size());
    return static_cast<bool>(in);
}

struct RecordOptions {
    std::string interfaceName;
    fs::path output;
    double seconds = 10.0;
    std::uint32_t idBase = 0x680;
};

std::optional<RecordOptions> ParseRecord(const std::vector<std::string>& args,
                                         std::string& error) {
    RecordOptions options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        auto value = [&](const char* option) -> std::optional<std::string> {
            if (++i >= args.size()) { error = std::string("missing value for ") + option; return {}; }
            return args[i];
        };
        if (args[i] == "--interface") {
            auto v = value("--interface"); if (!v) return {}; options.interfaceName = *v;
        } else if (args[i] == "--output") {
            auto v = value("--output"); if (!v) return {}; options.output = *v;
        } else if (args[i] == "--seconds") {
            auto v = value("--seconds"); if (!v) return {};
            try { options.seconds = std::stod(*v); } catch (...) { error = "invalid --seconds"; return {}; }
        } else if (args[i] == "--id-base") {
            auto v = value("--id-base"); if (!v) return {};
            try { options.idBase = static_cast<std::uint32_t>(std::stoul(*v, nullptr, 0)); }
            catch (...) { error = "invalid --id-base"; return {}; }
        } else { error = "unknown trace record option: " + args[i]; return {}; }
    }
    if (options.interfaceName.empty() || options.output.empty()) {
        error = "trace record requires --interface and --output";
        return {};
    }
    if (options.seconds < 0.0 || options.idBase > 0x7FCU) {
        error = "--seconds must be >= 0 and --id-base must leave four standard IDs";
        return {};
    }
    return options;
}

#ifdef __linux__
volatile std::sig_atomic_t gStop = 0;
void StopCapture(int) { gStop = 1; }

int Record(const RecordOptions& options) {
    const int socketFd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (socketFd < 0) { std::cerr << "rte: could not open SocketCAN socket\n"; return 4; }
    const int enableFd = 1;
    if (setsockopt(socketFd, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &enableFd, sizeof(enableFd)) != 0) {
        std::cerr << "rte: CAN interface/driver does not support CAN-FD\n";
        close(socketFd); return 4;
    }
    can_filter filters[4]{};
    for (std::uint32_t i = 0; i < 4; ++i) {
        filters[i].can_id = options.idBase + i;
        filters[i].can_mask = CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG;
    }
    setsockopt(socketFd, SOL_CAN_RAW, CAN_RAW_FILTER, filters, sizeof(filters));
    ifreq request{};
    std::strncpy(request.ifr_name, options.interfaceName.c_str(), IFNAMSIZ - 1);
    if (ioctl(socketFd, SIOCGIFINDEX, &request) != 0) {
        std::cerr << "rte: CAN interface not found: " << options.interfaceName << '\n';
        close(socketFd); return 3;
    }
    sockaddr_can address{};
    address.can_family = AF_CAN;
    address.can_ifindex = request.ifr_ifindex;
    if (bind(socketFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        std::cerr << "rte: could not bind CAN interface " << options.interfaceName << '\n';
        close(socketFd); return 4;
    }
    std::ofstream output(options.output, std::ios::binary | std::ios::trunc);
    if (!output) { std::cerr << "rte: could not create " << options.output << '\n'; close(socketFd); return 4; }
    output.write(kMagic.data(), kMagic.size());
    const auto start = std::chrono::steady_clock::now();
    gStop = 0;
    const auto previous = std::signal(SIGINT, StopCapture);
    std::uint64_t frames = 0;
    while (!gStop) {
        const auto beforeRead = std::chrono::steady_clock::now();
        if (options.seconds > 0.0 &&
            std::chrono::duration<double>(beforeRead - start).count() >= options.seconds) break;
        pollfd descriptor{socketFd, POLLIN, 0};
        const int ready = poll(&descriptor, 1, 100);
        if (ready <= 0 || (descriptor.revents & POLLIN) == 0) continue;
        canfd_frame frame{};
        const ssize_t count = read(socketFd, &frame, sizeof(frame));
        if (count < 0) continue;
        if (count != CANFD_MTU || frame.len != IVP_TRACE_PAYLOAD_SIZE ||
            ivp_trace_frame_type(frame.data) == 0) continue;
        const auto now = std::chrono::steady_clock::now();
        CaptureRecord record;
        record.hostNs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count());
        record.canId = frame.can_id & CAN_SFF_MASK;
        record.length = frame.len;
        std::memcpy(record.payload.data(), frame.data, frame.len);
        if (!WriteRecord(output, record)) { std::cerr << "rte: capture write failed\n"; break; }
        ++frames;
        if (options.seconds > 0.0 &&
            std::chrono::duration<double>(now - start).count() >= options.seconds) break;
    }
    std::signal(SIGINT, previous);
    close(socketFd);
    output.close();
    std::cout << "captured " << frames << " trace frames to " << options.output << '\n';
    return output ? 0 : 4;
}
#else
int Record(const RecordOptions&) {
    std::cerr << "rte: live trace recording currently requires Linux SocketCAN; trace export is portable\n";
    return 3;
}
#endif

struct Schema { std::string name; float scale = 1.0f; };
struct SparsePoint {
    std::uint8_t captureId;
    std::uint32_t sampleSequence;
    float value;
};

int Export(const std::vector<std::string>& args) {
    fs::path inputPath, outputPath;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if ((args[i] == "--input" || args[i] == "--output") && i + 1 < args.size()) {
            const bool input = args[i] == "--input";
            if (input) inputPath = args[++i]; else outputPath = args[++i];
        } else { std::cerr << "rte: unknown or incomplete trace export option: " << args[i] << '\n'; return 2; }
    }
    if (inputPath.empty() || outputPath.empty()) {
        std::cerr << "rte: trace export requires --input and --output\n"; return 2;
    }
    std::ifstream input(inputPath, std::ios::binary);
    std::array<char, 8> magic{};
    input.read(magic.data(), magic.size());
    if (!input || magic != kMagic) { std::cerr << "rte: invalid .rtecap file\n"; return 4; }
    std::array<Schema, IVP_TRACE_MAX_CHANNELS> schemas{};
    std::array<std::vector<SparsePoint>, IVP_TRACE_MAX_CHANNELS> sparsePoints{};
    CaptureRecord record;
    while (ReadRecord(input, record)) {
        ivp_trace_schema_frame_t schema{};
        if (record.length == IVP_TRACE_PAYLOAD_SIZE &&
            ivp_trace_decode_schema(record.payload.data(), &schema)) {
            schemas[schema.channel].name = schema.name;
            schemas[schema.channel].scale = schema.scale;
            continue;
        }
        ivp_trace_event_frame_t event{};
        if (record.length == IVP_TRACE_PAYLOAD_SIZE &&
            ivp_trace_decode_event(record.payload.data(), &event)) {
            sparsePoints[event.channel].push_back(
                {event.capture_id, event.sample_sequence, event.value});
        }
    }
    for (std::size_t i = 0; i < schemas.size(); ++i)
        if (schemas[i].name.empty()) schemas[i].name = "channel" + std::to_string(i);

    input.clear();
    input.seekg(static_cast<std::streamoff>(kMagic.size()));
    std::ofstream output(outputPath, std::ios::trunc);
    if (!output) { std::cerr << "rte: could not create " << outputPath << '\n'; return 4; }
    output << "record_type,host_ns,capture_id,sample_sequence,sample_in_block,device_cycles";
    for (const auto& schema : schemas) output << ',' << schema.name;
    output << ",event_channel,event_name,event_value,snapshot,samples_dropped,frames_dropped\n";
    std::uint64_t rows = 0;
    while (ReadRecord(input, record)) {
        ivp_trace_data_frame_t data{};
        if (ivp_trace_decode_data(record.payload.data(), &data)) {
            for (std::size_t sample = 0; sample < IVP_TRACE_SAMPLES_PER_DATA_FRAME; ++sample) {
                std::uint32_t sampleCycles = data.first_sample_cycles;
                if (sample >= 1U) sampleCycles += data.delta_cycles_div8[0] * 8U;
                if (sample >= 2U) sampleCycles += data.delta_cycles_div8[1] * 8U;
                output << "data," << record.hostNs << ',' << unsigned(data.capture_id) << ','
                       << data.first_sample_sequence + sample << ',' << sample << ',' << sampleCycles;
                for (std::size_t channel = 0; channel < IVP_TRACE_CHANNELS; ++channel)
                    output << ',' << std::setprecision(9)
                           << data.samples[sample][channel] * schemas[channel].scale;
                for (std::size_t channel = IVP_TRACE_CHANNELS; channel < schemas.size(); ++channel) {
                    const auto& points = sparsePoints[channel];
                    const SparsePoint* held = nullptr;
                    for (auto point = points.rbegin(); point != points.rend(); ++point) {
                        if (point->captureId == data.capture_id &&
                            point->sampleSequence <= data.first_sample_sequence + sample) {
                            held = &*point;
                            break;
                        }
                    }
                    output << ',';
                    if (held != nullptr) output << std::setprecision(9) << held->value;
                }
                output << ",,,,,,\n";
                ++rows;
            }
            continue;
        }
        ivp_trace_event_frame_t event{};
        if (ivp_trace_decode_event(record.payload.data(), &event)) {
            output << "event," << record.hostNs << ',' << unsigned(event.capture_id)
                   << ',' << event.sample_sequence << ",,";
            for (std::size_t channel = 0; channel < schemas.size(); ++channel) output << ',';
            output << unsigned(event.channel) << ',' << schemas[event.channel].name << ','
                   << std::setprecision(9) << event.value
                   << ',' << (event.snapshot ? 1 : 0) << ",,\n";
            ++rows;
            continue;
        }
        ivp_trace_status_frame_t status{};
        if (ivp_trace_decode_status(record.payload.data(), &status)) {
            output << "status," << record.hostNs << ',' << unsigned(status.capture_id) << ",,,";
            for (std::size_t channel = 0; channel < schemas.size(); ++channel) output << ',';
            output << ",,,," << status.samples_dropped << ',' << status.frames_dropped << '\n';
            ++rows;
        }
    }
    std::cout << "exported " << rows << " rows to " << outputPath << '\n';
    return output ? 0 : 4;
}
} // namespace

int RunTraceCommand(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "rte: trace requires record or export\n";
        return 2;
    }
    const std::vector<std::string> options(args.begin() + 1, args.end());
    if (args[0] == "record") {
        std::string error;
        const auto parsed = ParseRecord(options, error);
        if (!parsed) { std::cerr << "rte: " << error << '\n'; return 2; }
        return Record(*parsed);
    }
    if (args[0] == "export") return Export(options);
    std::cerr << "rte: unknown trace subcommand: " << args[0] << '\n';
    return 2;
}
