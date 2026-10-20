// Structural tests for the RTESim host runtime: scenario/event parsing,
// workdir layout, v3 session-start schema, and host-source staging paths.
// None of these require PlantCodegenLib (the scenario-host run itself is
// exercised by the `rte sim` acceptance smoke, not by this unit suite).

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <limits>

#include "RTESim/Scenario.h"
#include "RTESim/SimRunner.h"

namespace fs = std::filesystem;

namespace {

TEST(ScenarioEventParse, ParsesSmokeEvents) {
    RTESim::ScenarioEvent ev;
    std::string error;

    ASSERT_TRUE(RTESim::ParseEventSpec("t=0.1 kind=reset", ev, error)) << error;
    EXPECT_DOUBLE_EQ(ev.t, 0.1);
    EXPECT_EQ(ev.kind, "reset");
    EXPECT_TRUE(ev.arg.empty());

    ASSERT_TRUE(RTESim::ParseEventSpec("t=1.0 kind=var-set arg=IqVar=150", ev, error)) << error;
    EXPECT_DOUBLE_EQ(ev.t, 1.0);
    EXPECT_EQ(ev.kind, "var-set");
    EXPECT_EQ(ev.arg, "IqVar=150");

    ASSERT_TRUE(RTESim::ParseEventSpec("t=2 kind=fault-inject arg=OpenGates", ev, error)) << error;
    EXPECT_EQ(ev.arg, "OpenGates");

    ASSERT_TRUE(RTESim::ParseEventSpec("t=2.5 kind=fault-clear", ev, error)) << error;
    EXPECT_TRUE(ev.arg.empty());

    ASSERT_TRUE(RTESim::ParseEventSpec("t=3 kind=dyno-set arg=-120", ev, error)) << error;
    EXPECT_EQ(ev.arg, "-120");
}

TEST(ScenarioEventParse, RejectsBadSpecs) {
    RTESim::ScenarioEvent ev;
    std::string error;
    EXPECT_FALSE(RTESim::ParseEventSpec("t=0.1", ev, error));
    EXPECT_FALSE(RTESim::ParseEventSpec("kind=reset", ev, error));
    EXPECT_FALSE(RTESim::ParseEventSpec("t=abc kind=reset", ev, error));
    EXPECT_FALSE(RTESim::ParseEventSpec("t=-1 kind=reset", ev, error));
    EXPECT_FALSE(RTESim::ParseEventSpec("t=1 kind=bogus", ev, error));
    EXPECT_FALSE(RTESim::ParseEventSpec("t=1 kind=var-set arg=IqVar", ev, error));
    EXPECT_FALSE(RTESim::ParseEventSpec("t=1 kind=var-set arg=IqVar=abc", ev, error));
    EXPECT_FALSE(RTESim::ParseEventSpec("t=1 kind=dyno-set arg=rpm", ev, error));
    EXPECT_FALSE(RTESim::ParseEventSpec("t=1 kind=fault-inject", ev, error));
    EXPECT_FALSE(RTESim::ParseEventSpec("t=1 kind=reset bogus=1", ev, error));
}

TEST(SupplyParse, ParsesAllFields) {
    RTESim::SupplyOverride supply;
    std::string error;
    ASSERT_TRUE(RTESim::ParseSupplySpec("v_set=144.7,i_limit=400,sink_limit=50,can_sink=1",
                                        supply, error)) << error;
    EXPECT_TRUE(supply.any());
    EXPECT_NEAR(supply.vSet, 144.7, 1e-12);
    EXPECT_NEAR(supply.iLimit, 400.0, 1e-12);
    EXPECT_NEAR(supply.sinkLimit, 50.0, 1e-12);
    EXPECT_TRUE(supply.canSink);

    ASSERT_TRUE(RTESim::ParseSupplySpec("can_sink=0", supply, error)) << error;
    EXPECT_FALSE(supply.canSink);
}

TEST(SupplyParse, RejectsBadSpecs) {
    RTESim::SupplyOverride supply;
    std::string error;
    EXPECT_FALSE(RTESim::ParseSupplySpec("v_set=abc", supply, error));
    EXPECT_FALSE(RTESim::ParseSupplySpec("bogus=1", supply, error));
    EXPECT_FALSE(RTESim::ParseSupplySpec("can_sink=yes", supply, error));
    EXPECT_FALSE(RTESim::ParseSupplySpec("v_set", supply, error));
}

TEST(DynoSeriesParse, ParsesAndOrders) {
    std::vector<RTESim::DynoStep> series;
    std::string error;
    ASSERT_TRUE(RTESim::ParseDynoSeries("0=500,1.5=-120,2=0", series, error)) << error;
    ASSERT_EQ(series.size(), 3u);
    EXPECT_DOUBLE_EQ(series[0].t, 0.0);
    EXPECT_DOUBLE_EQ(series[0].rpm, 500.0);
    EXPECT_DOUBLE_EQ(series[1].t, 1.5);
    EXPECT_DOUBLE_EQ(series[1].rpm, -120.0);
    EXPECT_DOUBLE_EQ(series[2].rpm, 0.0);
}

TEST(DynoSeriesParse, RejectsBadSpecs) {
    std::vector<RTESim::DynoStep> series;
    std::string error;
    EXPECT_FALSE(RTESim::ParseDynoSeries("", series, error));
    EXPECT_FALSE(RTESim::ParseDynoSeries("abc", series, error));
    EXPECT_FALSE(RTESim::ParseDynoSeries("1=x", series, error));
    EXPECT_FALSE(RTESim::ParseDynoSeries("=5", series, error));
}

/* --------------------------------------------------------------------------
 * v3 session-start schema (must load through twin.replay byte-for-byte)
 * -------------------------------------------------------------------------- */

TEST(SessionSchema, SessionStartLine) {
    EXPECT_EQ(RTESim::FormatSessionStartLine("sim_smoke", "rte-sim"),
              R"({"type":"session_start","format":"rte-runtime-session-jsonl","version":3,"t":0,"port":"rte-sim","mode":"sim","protocol":"legacy","scenario":"sim_smoke"})");
}

TEST(SessionSchema, EscapesQuotes) {
    EXPECT_EQ(RTESim::JStr("a\"b\\c"), "a\\\"b\\\\c");
}

TEST(SessionSchema, JNumFormatting) {
    EXPECT_EQ(RTESim::JNum(150.0), "150");
    EXPECT_EQ(RTESim::JNum(0.1), "0.1");
    EXPECT_EQ(RTESim::JNum(-120.5), "-120.5");
    EXPECT_EQ(RTESim::JNum(50), "50");
    EXPECT_EQ(RTESim::JNum(std::nan("")), "null");
    EXPECT_EQ(RTESim::JNum(std::numeric_limits<double>::infinity()), "null");
}

/* --------------------------------------------------------------------------
 * Work dir layout + staged host sources
 * -------------------------------------------------------------------------- */

TEST(SimLayout, WorkdirStructure) {
    const auto layout = RTESim::LayoutForWorkDir(fs::path("/tmp/x/work"));
    EXPECT_EQ(layout.sw, fs::path("/tmp/x/work/sw"));
    EXPECT_EQ(layout.plant, fs::path("/tmp/x/work/plant"));
    EXPECT_EQ(layout.host, fs::path("/tmp/x/work/host"));
    EXPECT_EQ(layout.binary, fs::path("/tmp/x/work/host/sim_host"));
}

TEST(SimStaging, HostSourcesPresent) {
    // everything SimRunner::StageHostSources copies must exist in the repo
    const fs::path res = RTESIM_RESOURCE_DIR;
    for (const fs::path& p : {res / "hostmain" / "main.cpp",
                              res / "shim" / "platform_shim.cpp",
                              res / "shim" / "shim_runtime.h",
                              res / "shim" / "meas_access.h",
                              res / "src" / "Scenario.cpp",
                              res / "include" / "RTESim" / "Scenario.h",
                              res / "fallback" / "plant_generated.h",
                              res / "fallback" / "plant_generated.cpp"}) {
        EXPECT_TRUE(fs::is_regular_file(p)) << "missing staged source: " << p;
    }
}

} // namespace
