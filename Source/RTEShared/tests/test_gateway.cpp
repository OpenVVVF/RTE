#include <GatewayApiServer.h>
#include <GatewayClient.h>
#include <Sha256.h>
#include <TelemetryStore.h>

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <condition_variable>
#include <cmath>
#include <limits>
#include <mutex>

using json = nlohmann::json;

namespace {

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

class GatewayApiTest : public ::testing::Test {
protected:
    GatewayApiTest()
        : api(updater, store, [] {
              rte::runtime::GatewayApiOptions options;
              options.port = 0;
              options.workerThreads = 8;
              options.maxStreams = 2;
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
