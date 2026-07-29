#pragma once
// Ported from InverterClientImGui/src/telemetry_protocol.{h,cpp} (class TelemetryClient).
//
// Same wire protocol and reader-thread structure as the original, but instead of
// the old snapshot() state-pull model this client pushes decoded values through
// std::function callbacks. All callbacks fire on the reader thread; the consumer
// is responsible for marshalling onto the GUI thread.
//
// Pure std C++20 + POSIX (termios). No Qt, no _WIN32 branches.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace NodeGUI::runtime {

class LegacyTelemetryClient {
public:
    struct Stats {
        float rxHz = 0.0f;
        float rxBytesPerSec = 0.0f;
        uint64_t goodFrames = 0;
        uint64_t badFrames = 0;
        uint64_t rejectCrc = 0;
        uint64_t rejectHdr = 0;
        uint64_t rejectLen = 0;
        uint64_t rejectPayloadParse = 0;
        uint64_t rejectUnknownId = 0;
        uint32_t lastSeq = 0;
    };

    LegacyTelemetryClient() = default;
    ~LegacyTelemetryClient();                       // stops
    LegacyTelemetryClient(const LegacyTelemetryClient&) = delete;
    LegacyTelemetryClient& operator=(const LegacyTelemetryClient&) = delete;

    bool start(const std::string& port);            // spawns reader thread
    void stop();                                    // joins thread, closes port
    void suspend();                                 // close port, idle reader (for flashing)
    void resume();                                  // clear suspend, reader reconnects
    bool isSuspended() const;
    bool sendLine(const std::string& line);         // text shell command

    // All callbacks fire on the reader thread; the consumer marshals.
    std::function<void(const std::string& key, float value, float tsec)> onF32;
    std::function<void(const std::string& key, const std::string& value)> onString;
    std::function<void(const std::string& line)> onConsole;
    std::function<void(const Stats&)> onStats;      // ~1 Hz

private:
    // POSIX serial port (termios), same configuration as the original SerialPort.
    class SerialPort {
    public:
        SerialPort() = default;
        ~SerialPort();

        bool open(const std::string& port, int baud = 460800);
        void close();
        bool isOpen() const;

        int  read(uint8_t* buf, int cap);
        bool write(const uint8_t* data, int n);
        bool drain();

    private:
#ifdef _WIN32
        void* h_ = nullptr; // HANDLE
#else
        int h_ = -1; // INVALID_SERIAL
#endif
    };

    void threadMain(const std::string& port);

    // Ingest helpers (reader thread only; the original appended "Locked" because
    // they ran under mtx_ protecting the snapshot state -- there is no shared
    // state anymore, so no lock and no suffix).
    void ingestF32(const std::string& key, float v, float tsec);
    void ingestStr(const std::string& key, const std::string& v);

    // Dynamic key registry (id -> (type,key)), reader thread only.
    struct KeyDef {
        uint8_t type = 0;
        std::string key;
    };
    void onDefine(uint16_t id, uint8_t type, const char* key, uint8_t key_len);
    bool lookupKey(uint16_t id, KeyDef& out) const;

    void parseDefinePayload(const uint8_t* payload, size_t len);
    void parseDataPayload(const uint8_t* payload, size_t len, float tsec);

    Stats st_;                                        // reader thread only
    std::unordered_map<uint16_t, KeyDef> id_to_key_;  // reader thread only

    // Long-string reassembly (reader thread only).
    struct PartialString {
        std::string buf;
        float last_tsec = 0.0f;
    };
    std::unordered_map<std::string, PartialString> partial_str_;

    std::atomic<bool> run_{false};
    std::atomic<bool> suspended_{false};
    std::thread thr_;

    std::mutex serial_mtx_;
    SerialPort serial_;
};

} // namespace NodeGUI::runtime
