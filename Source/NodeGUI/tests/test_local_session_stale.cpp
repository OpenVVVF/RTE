#include "runtime/LocalSessionServer.h"
#include "runtime/TelemetryStore.h"
#include "runtime/RuntimeSessionExporter.h"

#include <RTEAutomation/Session.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace {

TEST(TelemetryStore, MeasuresEachSignalsValueChangeRate) {
    NodeGUI::runtime::TelemetryStore store;
    for (int i = 0; i <= 200; ++i) {
        const float t = static_cast<float>(i) / 200.0f;
        store.AddF32("fast", static_cast<float>(i), t);
        store.AddF32("rpm", static_cast<float>(i / 4), t);
        store.AddF32("constant", 7.0f, t);
    }

    auto displays = store.SignalDisplays();
    ASSERT_TRUE(displays.at("fast").updateHz.has_value());
    ASSERT_TRUE(displays.at("rpm").updateHz.has_value());
    ASSERT_TRUE(displays.at("constant").updateHz.has_value());
    EXPECT_NEAR(*displays.at("fast").updateHz, 200.0, 0.1);
    EXPECT_NEAR(*displays.at("rpm").updateHz, 50.0, 0.1);
    EXPECT_EQ(*displays.at("constant").updateHz, 0.0);

    // A second report at the same source timestamp cannot add a change.
    store.AddF32("fast", 999.0f, 1.0f);
    EXPECT_NEAR(*store.SignalDisplays().at("fast").updateHz, 200.0, 0.1);

    // A signal can keep arriving while its value stops changing.
    for (int i = 201; i <= 400; ++i)
        store.AddF32("rpm", 50.0f, static_cast<float>(i) / 200.0f);
    EXPECT_EQ(*store.SignalDisplays().at("rpm").updateHz, 0.0);

    store.SetSuspended(true);
    EXPECT_EQ(*store.SignalDisplays().at("fast").updateHz, 0.0);
    store.ClearSession();
    EXPECT_TRUE(store.SignalDisplays().empty());
}

TEST(TelemetryStore, RetainsFullResolutionUntilBudgetThenCompactsOldest) {
    NodeGUI::runtime::TelemetryStore store(800);  // 100 float time/value pairs
    for (int i = 0; i < 80; ++i)
        store.AddF32("signal", static_cast<float>(i), static_cast<float>(i) * 0.01f);
    auto retention = store.GetSessionRetentionStats();
    EXPECT_EQ(retention.storedSamples, 80);
    EXPECT_EQ(retention.archiveGeneration, 0);
    std::vector<NodeGUI::runtime::SessionFloatSample> page;
    std::size_t total = 0;
    uint64_t generation = 0;
    ASSERT_TRUE(store.CopySessionHistoryPage("signal", 0, 100, page, total, generation));
    ASSERT_EQ(total, 80);
    for (int i = 0; i < 80; ++i) EXPECT_FLOAT_EQ(page[i].y, static_cast<float>(i));

    for (int i = 80; i < 160; ++i)
        store.AddF32("signal", static_cast<float>(i), static_cast<float>(i) * 0.01f);
    retention = store.GetSessionRetentionStats();
    EXPECT_LE(retention.storedSamples, 100);
    EXPECT_GT(retention.archiveGeneration, 0);
    ASSERT_TRUE(store.CopySessionHistoryPage("signal", 0, 200, page, total, generation));
    ASSERT_EQ(total, retention.storedSamples);
    EXPECT_FLOAT_EQ(page.back().y, 159.0f);
    for (std::size_t i = 1; i < page.size(); ++i) EXPECT_LT(page[i - 1].t, page[i].t);
}

TEST(TelemetryStore, ExportsBeyondOldTwelveThousandSampleLimitWithoutCopyingArchive) {
    NodeGUI::runtime::TelemetryStore store(200000);
    for (int i = 0; i < 13000; ++i)
        store.AddF32("signal", static_cast<float>(i), static_cast<float>(i) * 0.005f);
    EXPECT_EQ(store.GetSessionRetentionStats().storedSamples, 13000);

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("session.jsonl"));
    QString error;
    ASSERT_TRUE(NodeGUI::runtime::ExportRuntimeSession(
        path, store, NodeGUI::runtime::RuntimeSessionMetadata{}, error))
        << error.toStdString();
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text));
    std::size_t numericEvents = 0;
    double lastTime = -1.0;
    while (!file.atEnd()) {
        const auto line = nlohmann::json::parse(file.readLine().toStdString());
        if (line.value("type", "") == "telemetry") {
            ++numericEvents;
            EXPECT_GE(line["t"].get<double>(), lastTime);
            lastTime = line["t"].get<double>();
        } else if (line.value("type", "") == "session_end") {
            EXPECT_EQ(line["signal_summaries"]["signal"]["samples"], 13000);
        }
    }
    EXPECT_EQ(numericEvents, 13000);
}

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
    const auto retention = RequestWithEvents(app, *session, "device.retention", {}, error);
    ASSERT_TRUE(retention.has_value()) << error;
    EXPECT_GE((*retention)["retained_numeric_samples"].get<std::size_t>(), 5);
    EXPECT_GT((*retention)["numeric_budget_bytes"].get<std::size_t>(), 0);
    const auto page = RequestWithEvents(app, *session, "device.session_history_page",
        {{"signal", "isr_current"}, {"offset", 0}, {"limit", 10}}, error);
    ASSERT_TRUE(page.has_value()) << error;
    ASSERT_EQ((*page)["samples"].size(), 1);
    EXPECT_EQ((*page)["samples"][0]["value"], 7.0);

    store.AddString("control_state", "IDLE");
    store.AddString("fault_flags_hex", "0x00000000");
    store.AddString("fault_active_names", "none");
    store.AddString("main_fault_flags_hex", "0x00000000");
    store.AddString("main_fault_names", "none");
    store.AddString("coprocessor_fault_flags_hex", "0x00000010");
    store.AddString("coprocessor_fault_names", "MainHeartbeatLost");
    store.AddString("coprocessor_safety_state", "FAULT_LATCHED");
    store.AddString("coprocessor_clear_result", "refused");
    store.AddF32("coprocessor_status_age_ms", 100.0f, 0.1f);
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
    EXPECT_EQ((*controlStatus)["main_fault_names"], "none");
    EXPECT_EQ((*controlStatus)["main_fault_flags_hex"], "0x00000000");
    EXPECT_EQ((*controlStatus)["coprocessor_fault_names"], "MainHeartbeatLost");
    EXPECT_EQ((*controlStatus)["coprocessor_safety_state"], "FAULT_LATCHED");
    EXPECT_EQ((*controlStatus)["coprocessor_clear_result"], "refused");

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
