#include "runtime/LocalSessionServer.h"
#include "runtime/TelemetryStore.h"

#include <RTEAutomation/Session.h>

#include <QCoreApplication>
#include <QEventLoop>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace {

std::optional<nlohmann::json> RequestWithEvents(
    QCoreApplication& app, const RTEAutomation::SessionDescriptor& session,
    const std::string& method, const nlohmann::json& params,
    std::string& error) {
    std::atomic<bool> done{false};
    std::optional<nlohmann::json> result;
    std::thread worker([&] {
        result = RTEAutomation::RequestSession(session, method, params, &error);
        done.store(true);
    });
    while (!done.load()) {
        app.processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(1ms);
    }
    worker.join();
    return result;
}

TEST(RteStudioSession, StoppedSignalIsNotAZeroAndHistoryWindowExpires) {
    int argc = 1;
    char name[] = "rte-session-test";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    NodeGUI::runtime::TelemetryStore store;
    NodeGUI::runtime::LocalSessionServer server(store);
    std::string error;
    ASSERT_TRUE(server.Start(&error)) << error;
    const auto session = RTEAutomation::DiscoverSession({}, &error);
    ASSERT_TRUE(session.has_value()) << error;

    store.AddF32("isr_current", 7.0f, 0.0f);
    store.AddString("fw_manifest", R"({"schema":1,"signals":[{"name":"isr_current","domain":"tim_isr"}]})");
    store.AddString("fw_graph", "foc_demo");
    store.AddString("control_state", "RUNNING");
    store.AddF32("tim_isr_running", 1.0f, 0.0f);
    store.AddF32("control_outputs_enabled", 1.0f, 0.0f);
    store.AddF32("foc_running", 1.0f, 0.0f);
    store.AddF32("foc_id", 3.0f, 0.0f);
    store.SetStats(100, 1000, 1, 0, 0, 0, 0, 0, 0, 1);
    const auto live = RequestWithEvents(app, *session, "device.telemetry", {}, error);
    ASSERT_TRUE(live.has_value()) << error;
    EXPECT_EQ((*live)["signals"]["isr_current"], 7.0);
    EXPECT_EQ((*live)["signal_status"]["isr_current"]["state"], "live");

    store.AddString("control_state", "IDLE");
    store.AddF32("foc_running", 0.0f, 0.1f);
    store.AddF32("tim_isr_running", 1.0f, 0.1f);
    store.AddF32("control_outputs_enabled", 0.0f, 0.1f);
    store.AddF32("isr_current", 6.0f, 0.1f);
    store.SetStats(100, 1000, 2, 0, 0, 0, 0, 0, 0, 2);
    const auto stopped = RequestWithEvents(app, *session, "device.telemetry", {}, error);
    ASSERT_TRUE(stopped.has_value()) << error;
    EXPECT_EQ((*stopped)["signals"]["isr_current"], 6.0);
    EXPECT_EQ((*stopped)["signal_status"]["isr_current"]["state"], "live");
    EXPECT_TRUE((*stopped)["signals"]["foc_id"].is_null());
    EXPECT_EQ((*stopped)["signal_status"]["foc_id"]["state"], "foc_stopped");
    EXPECT_LT((*stopped)["signal_status"]["isr_current"]["age_s"].get<double>(), 2.0);

    const auto controlStatus = RequestWithEvents(
        app, *session, "device.control_status", {}, error);
    ASSERT_TRUE(controlStatus.has_value()) << error;
    EXPECT_EQ((*controlStatus)["state"], "IDLE");
    EXPECT_EQ((*controlStatus)["tim_isr_running"], 1.0);
    EXPECT_EQ((*controlStatus)["control_outputs_enabled"], 0.0);

    const auto stoppedSnapshot = RequestWithEvents(app, *session, "device.snapshot",
        {{"signals", {"isr_current", "foc_id"}}}, error);
    ASSERT_TRUE(stoppedSnapshot.has_value()) << error;
    EXPECT_EQ((*stoppedSnapshot)["values"]["isr_current"]["value"], 6.0);
    EXPECT_EQ((*stoppedSnapshot)["values"]["isr_current"]["state"], "live");

    store.AddF32("tim_isr_running", 0.0f, 0.15f);
    const auto isrStopped = RequestWithEvents(app, *session, "device.telemetry", {}, error);
    ASSERT_TRUE(isrStopped.has_value()) << error;
    EXPECT_TRUE((*isrStopped)["signals"]["isr_current"].is_null());
    EXPECT_EQ((*isrStopped)["signal_status"]["isr_current"]["state"], "isr_stopped");

    store.AddString("control_state", "RUNNING");
    store.AddF32("tim_isr_running", 1.0f, 0.2f);
    store.AddF32("foc_running", 1.0f, 0.2f);
    store.AddF32("isr_current", 7.0f, 0.2f);
    store.AddF32("foc_id", 3.0f, 0.2f);
    store.SetStats(100, 1000, 3, 0, 0, 0, 0, 0, 0, 3);

    // Keep the transport healthy while the ISR signal stops arriving.
    for (int i = 0; i < 23; ++i) {
        std::this_thread::sleep_for(100ms);
        store.SetStats(100, 1000, i + 4, 0, 0, 0, 0, 0, 0, i + 4);
    }
    const auto stale = RequestWithEvents(app, *session, "device.telemetry", {}, error);
    ASSERT_TRUE(stale.has_value()) << error;
    EXPECT_TRUE((*stale)["signals"]["isr_current"].is_null());
    EXPECT_EQ((*stale)["last_known_values"]["isr_current"], 7.0);
    EXPECT_EQ((*stale)["signal_status"]["isr_current"]["state"], "stopped_reporting");
    EXPECT_EQ((*stale)["strings"]["fw_graph"], "foc_demo");
    EXPECT_EQ((*stale)["signal_status"]["fw_graph"]["state"], "live");

    const auto histories = RequestWithEvents(app, *session, "device.histories",
        {{"signals", {"isr_current"}}, {"window_s", 1.0}}, error);
    ASSERT_TRUE(histories.has_value()) << error;
    EXPECT_TRUE((*histories)["series"]["isr_current"].empty());
    EXPECT_GT((*histories)["window_end_s"].get<double>(), 2.0);

    // A real zero is distinct from no current measurement.
    store.AddF32("isr_current", 0.0f, 2.4f);
    store.SetStats(100, 1000, 27, 0, 0, 0, 0, 0, 0, 27);
    const auto resumed = RequestWithEvents(app, *session, "device.telemetry", {}, error);
    ASSERT_TRUE(resumed.has_value()) << error;
    EXPECT_EQ((*resumed)["signals"]["isr_current"], 0.0);
    EXPECT_EQ((*resumed)["signal_status"]["isr_current"]["state"], "live");
}

}  // namespace
