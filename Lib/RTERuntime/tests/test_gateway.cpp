#include <GatewayApiServer.h>
#include <GatewayClient.h>
#include <LegacyTelemetryClient.h>
#include <Sha256.h>
#include <TelemetryStore.h>

#include <inverter_protocol/protocol.h>

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <condition_variable>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>

#include <fcntl.h>
#include <unistd.h>

using json = nlohmann::json;

namespace {

std::vector<uint8_t> LegacyFrame(uint8_t messageType,
                                 uint32_t sequence,
                                 const std::vector<uint8_t>& payload) {
#pragma pack(push, 1)
    struct Header {
        uint32_t magic;
        uint8_t version;
        uint8_t messageType;
        uint16_t payloadLength;
        uint32_t sequence;
        uint32_t timeUs;
    };
#pragma pack(pop)
    const Header header{0x544C4D31U, 1, messageType,
                        static_cast<uint16_t>(payload.size()), sequence, 0};
    std::vector<uint8_t> decoded(sizeof(header) + payload.size() + 2);
    std::memcpy(decoded.data(), &header, sizeof(header));
    std::copy(payload.begin(), payload.end(), decoded.begin() + sizeof(header));
    const uint16_t crc = ivp_crc16_ccitt(decoded.data(), decoded.size() - 2);
    decoded[decoded.size() - 2] = static_cast<uint8_t>(crc & 0xffU);
    decoded[decoded.size() - 1] = static_cast<uint8_t>(crc >> 8U);
    std::vector<uint8_t> encoded(decoded.size() + decoded.size() / 254 + 2);
    const std::size_t length =
        ivp_cobs_encode(decoded.data(), decoded.size(), encoded.data(), encoded.size());
    encoded.resize(length + 1);
    encoded[length] = 0;
    return encoded;
}

TEST(Sha256, MatchesKnownVector) {
    const std::string input = "abc";
    EXPECT_EQ(rte::runtime::Sha256Hex(input.data(), input.size()),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(TelemetryStore, ReplaysOrderedEventsAndReportsGaps) {
    rte::runtime::TelemetryStore store;
    store.AddF32("voltage", 12.5f, 1.0f);
    store.AddString("mode", "run");
    store.AddConsoleLine("ready");

    bool reset = false;
    const auto events = store.EventsSince(0, 10, reset);
    ASSERT_FALSE(reset);
    ASSERT_EQ(events.size(), 3U);
    EXPECT_LT(events[0].seq, events[1].seq);
    EXPECT_LT(events[1].seq, events[2].seq);
    EXPECT_EQ(events[0].kind, rte::runtime::TelemetryEventKind::Float);
    EXPECT_EQ(events[2].text, "ready");
}

TEST(TelemetryStore, StartsANewPlotEpochWhenSourceTimeMovesBackward) {
    rte::runtime::TelemetryStore store;
    store.AddF32("voltage", 10.0f, 10.0f);
    store.AddF32("voltage", 11.0f, 11.0f);
    store.AddF32("voltage", 12.0f, 1.0f);

    std::deque<float> times;
    std::deque<float> values;
    ASSERT_TRUE(store.CopyHistory("voltage", times, values));
    ASSERT_EQ(times.size(), 1U);
    EXPECT_FLOAT_EQ(times.front(), 1.0f);
    EXPECT_FLOAT_EQ(values.front(), 12.0f);

    const auto session = store.SessionSnapshot();
    const auto& sessionTimes = session.floatSignals.at("voltage").t;
    ASSERT_EQ(sessionTimes.size(), 3U);
    EXPECT_LE(sessionTimes[0], sessionTimes[1]);
    EXPECT_LE(sessionTimes[1], sessionTimes[2]);
}

TEST(TelemetryStore, ResetsOnlyLiveTelemetryAfterAReplayGap) {
    rte::runtime::TelemetryStore store;
    store.AddF32("voltage", 10.0f, 1.0f);
    store.AddConsoleLine("preserve me");
    store.ResetLiveTelemetry();

    EXPECT_TRUE(store.SignalNames().empty());
    EXPECT_EQ(store.ConsoleSince(0).size(), 1U);
    const auto session = store.SessionSnapshot();
    EXPECT_EQ(session.floatSignals.at("voltage").y.size(), 1U);
    EXPECT_EQ(session.console.size(), 1U);
}

TEST(LegacyTelemetryClient, DecodesTelemetryFromAPseudoTerminal) {
    const int master = posix_openpt(O_RDWR | O_NOCTTY);
    ASSERT_GE(master, 0);
    ASSERT_EQ(grantpt(master), 0);
    ASSERT_EQ(unlockpt(master), 0);
    const char* slaveName = ptsname(master);
    ASSERT_NE(slaveName, nullptr);

    rte::runtime::LegacyTelemetryClient telemetry;
    std::mutex mutex;
    std::condition_variable changed;
    bool transportConnected = false;
    bool received = false;
    telemetry.onConnection = [&](bool connected) {
        std::lock_guard lock(mutex);
        transportConnected = connected;
        changed.notify_all();
    };
    telemetry.onF32 = [&](const std::string& key, float value, float) {
        if (key != "voltage" || value != 42.5f) return;
        std::lock_guard lock(mutex);
        received = true;
        changed.notify_all();
    };
    ASSERT_TRUE(telemetry.start(slaveName));
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(2),
                                     [&] { return transportConnected; }));
    }

    std::vector<uint8_t> definition{1, 1, 0, 1, 7};
    definition.insert(definition.end(), {'v', 'o', 'l', 't', 'a', 'g', 'e'});
    const auto definitionFrame = LegacyFrame(2, 1, definition);
    ASSERT_EQ(write(master, definitionFrame.data(), definitionFrame.size()),
              static_cast<ssize_t>(definitionFrame.size()));

    const float value = 42.5f;
    std::vector<uint8_t> sample{1, 1, 0, 1};
    const auto* valueBytes = reinterpret_cast<const uint8_t*>(&value);
    sample.insert(sample.end(), valueBytes, valueBytes + sizeof(value));
    const auto sampleFrame = LegacyFrame(1, 2, sample);
    ASSERT_EQ(write(master, sampleFrame.data(), sampleFrame.size()),
              static_cast<ssize_t>(sampleFrame.size()));

    {
        std::unique_lock lock(mutex);
        EXPECT_TRUE(changed.wait_for(lock, std::chrono::seconds(2),
                                     [&] { return received; }));
    }
    telemetry.stop();
    close(master);
}

class GatewayApiTest : public ::testing::Test {
protected:
    GatewayApiTest()
        : store(30.0f, 12000, 6000, 3),
          api(updater, store, [] {
              rte::runtime::GatewayApiOptions options;
              options.port = 0;
              options.workerThreads = 8;
              options.maxStreams = 2;
              options.maxFirmwareBytes = 1024;
              options.leaseTtl = std::chrono::seconds(1);
              return options;
          }()) {}

    void SetUp() override {
        api.setDevicePort("/dev/test");
        api.setCommandHandler([this](const std::string& command) {
            store.AddConsoleLine("ack: " + command);
            return true;
        });
        ASSERT_TRUE(api.start());
        client = std::make_unique<httplib::Client>("http://127.0.0.1:"
                                                   + std::to_string(api.actualPort()));
    }

    void TearDown() override { api.stop(); }

    rte::runtime::TelemetryStore store;
    rte::runtime::FirmwareUpdater updater;
    rte::runtime::GatewayApiServer api;
    std::unique_ptr<httplib::Client> client;
};

TEST_F(GatewayApiTest, EnforcesSingleOperatorLeaseForCommands) {
    const auto rejected = client->Post("/api/v1/commands",
                                       json({{"command", "status"}}).dump(),
                                       "application/json");
    ASSERT_TRUE(rejected);
    EXPECT_EQ(rejected->status, 409);

    const auto acquired = client->Post("/api/v1/control/lease",
                                       json({{"owner", "test-one"}}).dump(),
                                       "application/json");
    ASSERT_TRUE(acquired);
    ASSERT_EQ(acquired->status, 201);
    const std::string lease = json::parse(acquired->body).at("id");

    const auto conflict = client->Post("/api/v1/control/lease",
                                       json({{"owner", "test-two"}}).dump(),
                                       "application/json");
    ASSERT_TRUE(conflict);
    EXPECT_EQ(conflict->status, 409);

    const httplib::Headers headers{{"X-RTE-Lease-ID", lease}};
    const auto command = client->Post("/api/v1/commands", headers,
                                      json({{"command", "status"}, {"wait_ms", 0}}).dump(),
                                      "application/json");
    ASSERT_TRUE(command);
    EXPECT_EQ(command->status, 200);
    EXPECT_EQ(json::parse(command->body)["lines"][0]["text"], "ack: status");
}

TEST_F(GatewayApiTest, ExpiresAnUnrenewedOperatorLease) {
    const auto acquired = client->Post("/api/v1/control/lease",
                                       json({{"owner", "short-lived"}}).dump(),
                                       "application/json");
    ASSERT_TRUE(acquired);
    ASSERT_EQ(acquired->status, 201);

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    const auto status = client->Get("/api/v1/control/lease");
    ASSERT_TRUE(status);
    ASSERT_EQ(status->status, 200);
    EXPECT_FALSE(json::parse(status->body).value("held", true));
}

TEST_F(GatewayApiTest, ClientRenewsOwnedLeaseWithoutStartingEventStream) {
    const std::string url = "http://127.0.0.1:" + std::to_string(api.actualPort());
    rte::runtime::GatewayClient leaseClient(url);
    std::string error;
    ASSERT_TRUE(leaseClient.acquireLease("long-operation", &error)) << error;
    EXPECT_FALSE(leaseClient.eventsRunning());

    std::this_thread::sleep_for(std::chrono::milliseconds(1400));
    std::vector<std::string> output;
    EXPECT_TRUE(leaseClient.sendCommand("still-owned", &output, &error, 0)) << error;
    ASSERT_EQ(output.size(), 1U);
    EXPECT_EQ(output.front(), "ack: still-owned");
}

TEST_F(GatewayApiTest, ClientRenewsSuppliedLeaseDuringLongOperations) {
    const auto acquired = client->Post("/api/v1/control/lease",
                                       json({{"owner", "shared-operation"}}).dump(),
                                       "application/json");
    ASSERT_TRUE(acquired);
    ASSERT_EQ(acquired->status, 201);

    const std::string url = "http://127.0.0.1:" + std::to_string(api.actualPort());
    rte::runtime::GatewayClient leaseClient(url);
    leaseClient.useLease(json::parse(acquired->body).at("id"), "shared-operation");
    std::this_thread::sleep_for(std::chrono::milliseconds(1400));

    std::string error;
    EXPECT_TRUE(leaseClient.sendCommand("shared-still-owned", nullptr, &error, 0)) << error;
}

TEST_F(GatewayApiTest, ClientNotifiesWhenItsLeaseIsLost) {
    const std::string url = "http://127.0.0.1:" + std::to_string(api.actualPort());
    rte::runtime::GatewayClient leaseClient(url);
    std::mutex mutex;
    std::condition_variable changed;
    bool lost = false;
    leaseClient.onLease = [&](bool owned, const std::string&) {
        if (owned) return;
        std::lock_guard lock(mutex);
        lost = true;
        changed.notify_all();
    };
    std::string error;
    ASSERT_TRUE(leaseClient.acquireLease("loss-test", &error)) << error;
    const auto removed = client->Delete(
        "/api/v1/control/lease/" + leaseClient.leaseId());
    ASSERT_TRUE(removed);
    ASSERT_EQ(removed->status, 200);
    {
        std::unique_lock lock(mutex);
        EXPECT_TRUE(changed.wait_for(lock, std::chrono::seconds(2),
                                     [&] { return lost; }));
    }
    EXPECT_FALSE(leaseClient.hasLease());
}

TEST_F(GatewayApiTest, RejectsCorruptFirmwareBeforeQueuing) {
    const auto acquired = client->Post("/api/v1/control/lease",
                                       json({{"owner", "test"}}).dump(),
                                       "application/json");
    ASSERT_TRUE(acquired);
    const std::string lease = json::parse(acquired->body).at("id");
    const httplib::Headers headers{{"X-RTE-Lease-ID", lease},
                                   {"X-RTE-SHA256", std::string(64, '0')},
                                   {"X-RTE-Filename", "firmware.bin"}};
    const auto response = client->Post("/api/v1/flash/jobs", headers,
                                       "firmware", "application/octet-stream");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 400);
    EXPECT_EQ(json::parse(response->body)["error"]["code"], "digest_mismatch");
    EXPECT_FALSE(updater.status().busy);
}

TEST_F(GatewayApiTest, ReturnsStructuredErrorsForRoutingAndPayloadLimits) {
    const auto missing = client->Get("/api/v1/not-a-route");
    ASSERT_TRUE(missing);
    ASSERT_EQ(missing->status, 404);
    EXPECT_EQ(json::parse(missing->body)["error"]["code"], "not_found");

    const auto oversized = client->Post("/api/v1/flash/jobs",
                                        std::string(2048, 'x'),
                                        "application/octet-stream");
    ASSERT_TRUE(oversized);
    ASSERT_EQ(oversized->status, 413);
    EXPECT_EQ(json::parse(oversized->body)["error"]["code"],
              "payload_too_large");
}

TEST_F(GatewayApiTest, GatewayClientUploadsTheExactFirmwareBytesAndDigest) {
    namespace fs = std::filesystem;
    const std::string firmware{"\x00\x01\x7f\x80\xffRTE\x00", 9};
    const fs::path path = fs::temp_directory_path() / "rte-upload-integrity-test.bin";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(firmware.data(), static_cast<std::streamsize>(firmware.size()));
    }

    const std::string url = "http://127.0.0.1:" + std::to_string(api.actualPort());
    rte::runtime::GatewayClient flashClient(url);
    std::string error;
    ASSERT_TRUE(flashClient.acquireLease("upload-test", &error)) << error;
    std::string job;
    ASSERT_TRUE(flashClient.flashFile(path.string(), false, &job, &error)) << error;
    EXPECT_FALSE(job.empty());

    const json state = json::parse(flashClient.stateJson(&error));
    EXPECT_EQ(state["flash"]["sha256"],
              rte::runtime::Sha256Hex(firmware.data(), firmware.size()));
    EXPECT_EQ(state["flash"]["filename"], path.filename().string());
    std::error_code removeError;
    fs::remove(path, removeError);
}

TEST_F(GatewayApiTest, ReportsGatewayAndPhysicalDeviceHealthSeparately) {
    const auto disconnected = client->Get("/api/v1/health");
    ASSERT_TRUE(disconnected);
    ASSERT_EQ(disconnected->status, 200);
    EXPECT_FALSE(json::parse(disconnected->body)["ok"]);
    EXPECT_FALSE(json::parse(disconnected->body)["device"]["connected"]);

    api.setDeviceConnected(true);
    const auto connected = client->Get("/api/v1/health");
    ASSERT_TRUE(connected);
    EXPECT_TRUE(json::parse(connected->body)["ok"]);
    EXPECT_TRUE(json::parse(connected->body)["device"]["connected"]);
}

TEST_F(GatewayApiTest, StreamsPhysicalDeviceConnectionChanges) {
    const std::string url = "http://127.0.0.1:" + std::to_string(api.actualPort());
    rte::runtime::GatewayClient eventClient(url);
    std::mutex mutex;
    std::condition_variable changed;
    bool connected = false;
    eventClient.onDevice = [&](bool value, const std::string& port) {
        if (!value || port != "/dev/test") return;
        std::lock_guard lock(mutex);
        connected = true;
        changed.notify_all();
    };
    eventClient.startEvents();
    api.setDeviceConnected(true);
    {
        std::unique_lock lock(mutex);
        EXPECT_TRUE(changed.wait_for(lock, std::chrono::seconds(2),
                                     [&] { return connected; }));
    }
    eventClient.stopEvents();
}

TEST_F(GatewayApiTest, KeepsOnlyReadOnlyLegacyCompatibility) {
    const auto info = client->Get("/api/info");
    ASSERT_TRUE(info);
    EXPECT_EQ(info->status, 200);
    EXPECT_TRUE(json::parse(info->body)["deprecated"]);

    const auto mutation = client->Post("/flash", "firmware",
                                       "application/octet-stream");
    ASSERT_TRUE(mutation);
    EXPECT_EQ(mutation->status, 410);
}

TEST_F(GatewayApiTest, StreamsDecodedEventsToMultipleObservers) {
    const std::string url = "http://127.0.0.1:" + std::to_string(api.actualPort());
    rte::runtime::GatewayClient first(url);
    rte::runtime::GatewayClient second(url);
    std::mutex mutex;
    std::condition_variable changed;
    int received = 0;
    auto observe = [&](const std::string& signal, float value, float timestamp) {
        if (signal != "voltage" || value != 42.0f || timestamp != 3.0f) return;
        std::lock_guard lock(mutex);
        ++received;
        changed.notify_all();
    };
    first.onF32 = observe;
    second.onF32 = observe;
    first.startEvents();
    second.startEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    store.AddF32("voltage", 42.0f, 3.0f);

    {
        std::unique_lock lock(mutex);
        EXPECT_TRUE(changed.wait_for(lock, std::chrono::seconds(2),
                                     [&] { return received == 2; }));
    }
    first.stopEvents();
    second.stopEvents();
}

TEST_F(GatewayApiTest, KeepsRequestsResponsiveWhileObserversAreStreaming) {
    const std::string url = "http://127.0.0.1:" + std::to_string(api.actualPort());
    rte::runtime::GatewayClient first(url);
    rte::runtime::GatewayClient second(url);
    std::mutex mutex;
    std::condition_variable changed;
    int connected = 0;
    auto connection = [&](bool value, const std::string&) {
        if (!value) return;
        std::lock_guard lock(mutex);
        ++connected;
        changed.notify_all();
    };
    first.onConnection = connection;
    second.onConnection = connection;
    first.startEvents();
    second.startEvents();
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(2),
                                     [&] { return connected == 2; }));
    }
    const auto health = client->Get("/api/v1/health");
    ASSERT_TRUE(health);
    EXPECT_EQ(health->status, 200);
    first.stopEvents();
    second.stopEvents();
}

TEST_F(GatewayApiTest, SnapshotPreservesLatestSampleTimestamp) {
    store.AddF32("voltage", 42.0f, 12.5f);
    const auto state = client->Get("/api/v1/state");
    ASSERT_TRUE(state);
    ASSERT_EQ(state->status, 200);
    EXPECT_FLOAT_EQ(json::parse(state->body)["latest_timestamps"]["voltage"], 12.5f);

    const std::string url = "http://127.0.0.1:" + std::to_string(api.actualPort());
    rte::runtime::GatewayClient eventClient(url);
    std::mutex mutex;
    std::condition_variable changed;
    float receivedTimestamp = -1.0f;
    eventClient.onF32 = [&](const std::string& signal, float, float timestamp) {
        if (signal != "voltage") return;
        std::lock_guard lock(mutex);
        receivedTimestamp = timestamp;
        changed.notify_all();
    };
    eventClient.startEvents();
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(2),
                                     [&] { return receivedTimestamp >= 0.0f; }));
    }
    eventClient.stopEvents();
    EXPECT_FLOAT_EQ(receivedTimestamp, 12.5f);
}

TEST_F(GatewayApiTest, ReconnectReplaysMissedTelemetryInTimestampOrder) {
    const std::string url = "http://127.0.0.1:" + std::to_string(api.actualPort());
    rte::runtime::GatewayClient eventClient(url);
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<float> values;
    eventClient.onF32 = [&](const std::string& signal, float value, float) {
        if (signal != "voltage") return;
        std::lock_guard lock(mutex);
        values.push_back(value);
        changed.notify_all();
    };
    eventClient.startEvents();
    store.AddF32("voltage", 1.0f, 1.0f);
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(2),
                                     [&] { return !values.empty(); }));
    }
    eventClient.stopEvents();
    {
        std::lock_guard lock(mutex);
        values.clear();
    }

    store.AddF32("voltage", 2.0f, 2.0f);
    store.AddF32("voltage", 3.0f, 3.0f);
    eventClient.startEvents();
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(2),
                                     [&] { return values.size() >= 2; }));
    }
    eventClient.stopEvents();

    std::lock_guard lock(mutex);
    ASSERT_EQ(values.size(), 2U);
    EXPECT_FLOAT_EQ(values[0], 2.0f);
    EXPECT_FLOAT_EQ(values[1], 3.0f);
}

TEST_F(GatewayApiTest, ReconnectResetsClientAfterReplayGap) {
    const std::string url = "http://127.0.0.1:" + std::to_string(api.actualPort());
    rte::runtime::GatewayClient eventClient(url);
    std::mutex mutex;
    std::condition_variable changed;
    bool receivedFirst = false;
    bool reset = false;
    float latest = 0.0f;
    eventClient.onReset = [&] {
        std::lock_guard lock(mutex);
        reset = true;
        changed.notify_all();
    };
    eventClient.onF32 = [&](const std::string& signal, float value, float) {
        if (signal != "voltage") return;
        std::lock_guard lock(mutex);
        latest = value;
        if (value == 1.0f) receivedFirst = true;
        changed.notify_all();
    };
    eventClient.startEvents();
    store.AddF32("voltage", 1.0f, 1.0f);
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(2),
                                     [&] { return receivedFirst; }));
    }
    eventClient.stopEvents();
    for (int value = 2; value <= 6; ++value) {
        store.AddF32("voltage", static_cast<float>(value),
                     static_cast<float>(value));
    }
    eventClient.startEvents();
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(2), [&] {
            return reset && latest == 6.0f;
        }));
    }
    eventClient.stopEvents();
}

TEST_F(GatewayApiTest, PreservesNonFiniteTelemetryWithoutCrashingEventClient) {
    store.AddF32("unstable", std::numeric_limits<float>::quiet_NaN(), 7.0f);
    const std::string url = "http://127.0.0.1:" + std::to_string(api.actualPort());
    rte::runtime::GatewayClient eventClient(url);
    std::mutex mutex;
    std::condition_variable changed;
    bool received = false;
    eventClient.onF32 = [&](const std::string& signal, float value, float) {
        if (signal != "unstable" || !std::isnan(value)) return;
        std::lock_guard lock(mutex);
        received = true;
        changed.notify_all();
    };
    eventClient.startEvents();
    {
        std::unique_lock lock(mutex);
        EXPECT_TRUE(changed.wait_for(lock, std::chrono::seconds(2),
                                     [&] { return received; }));
    }
    eventClient.stopEvents();
}

}  // namespace
