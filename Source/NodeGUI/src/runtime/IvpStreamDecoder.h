#pragma once

#include <inverter_protocol/host/host_client.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace NodeGUI::runtime {

// Transport-independent InverterProtocol stream decoder. FeedBytes() consumes
// a raw byte stream of COBS-encoded, 0x00-delimited IVP frames (exactly what
// HostSim --live publishes on TCP, or what a serial link carries) and fires
// the registered callbacks for each decoded telemetry value. No I/O, no Qt,
// no threads — callbacks fire synchronously from FeedBytes()/EmitStats() on
// the caller's thread. Kept std-only so it is directly unit-testable.
//
// The wiring mirrors ivp::InverterClient: keys come from DEFINE frames, the
// string key "print" is console text, other strings reach onStringValue, and
// EmitStats(delta_seconds) refreshes the rolling RX rate/byte counters.
class IvpStreamDecoder {
public:
    // Same callback signatures as ivp::InverterClient so consumers can treat
    // both transports identically.
    using F32Callback = ivp::InverterClient::F32Callback;
    using StringCallback = ivp::InverterClient::StringCallback;
    using ConsoleCallback = ivp::InverterClient::ConsoleCallback;
    using StatsCallback = ivp::InverterClient::StatsCallback;

    F32Callback onF32Value;
    StringCallback onStringValue;
    ConsoleCallback onConsoleLine;
    StatsCallback onStats;

    // Consumes stream bytes. Safe to call with partial frames.
    void FeedBytes(const uint8_t* data, size_t n);

    // Call ~1 Hz with the elapsed seconds since the last call; updates
    // rx_hz/rx_bytes_per_sec and fires onStats. Only depends on stats
    // gathered by FeedBytes, so it keeps ticking when the link is quiet.
    void EmitStats(double dt_seconds);

    const ivp::ClientStats& Stats() const { return stats_; }

private:
    static constexpr size_t kMaxEncodedFrame = 4096;

    struct KeyDef {
        uint8_t type = 0;
        std::string key;
    };
    struct PartialString {
        std::string buf;
        uint32_t lastTimeUs = 0;
    };

    void HandleFrame(const uint8_t* encoded, size_t len);
    void HandleDefine(const uint8_t* payload, uint16_t payload_len);
    void HandleData(const uint8_t* payload, uint16_t payload_len, uint32_t time_us);
    void HandleCommandResponse(const uint8_t* payload, uint16_t payload_len);
    void IngestString(uint16_t id, const std::string& key, const std::string& value,
                      uint32_t time_us);

    uint8_t frameBuf_[kMaxEncodedFrame] = {};
    size_t frameLen_ = 0;

    ivp::ClientStats stats_;
    uint64_t framesInWindow_ = 0;
    uint64_t bytesInWindow_ = 0;

    std::unordered_map<uint16_t, KeyDef> registry_;
    std::unordered_map<std::string, PartialString> partialStrings_;
};

}  // namespace NodeGUI::runtime
