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

TEST(ScenarioEventParse, ParsesV11Kinds) {
    RTESim::ScenarioEvent ev;
    std::string error;

    // F36 additions
    ASSERT_TRUE(RTESim::ParseEventSpec("t=1.0 kind=stop", ev, error)) << error;
    EXPECT_EQ(ev.kind, "stop");
    ASSERT_TRUE(RTESim::ParseEventSpec("t=1.0 kind=gate-ready arg=0", ev, error)) << error;
    EXPECT_EQ(ev.arg, "0");
    ASSERT_TRUE(RTESim::ParseEventSpec("t=1.0 kind=gate-ready arg=1", ev, error)) << error;

    // F13/F7: plant-input carries the same Name=Value grammar as var-set
    ASSERT_TRUE(RTESim::ParseEventSpec("t=1.0 kind=plant-input arg=Omega_Cmd=188.5",
                                       ev, error)) << error;
    EXPECT_EQ(ev.kind, "plant-input");
    EXPECT_EQ(ev.arg, "Omega_Cmd=188.5");
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
    EXPECT_FALSE(RTESim::ParseEventSpec("t=1 kind=var-set arg=IqVar=nan", ev, error));
    EXPECT_FALSE(RTESim::ParseEventSpec("t=1 kind=var-set arg=IqVar=inf", ev, error));
}

TEST(ScenarioEventParse, RejectsBadV11Specs) {
    RTESim::ScenarioEvent ev;
    std::string error;
    // F01: unknown fault-inject names die at parse time listing the valid set
    EXPECT_FALSE(RTESim::ParseEventSpec("t=1 kind=fault-inject arg=Bogus", ev, error));
    EXPECT_NE(error.find("OpenGates"), std::string::npos);
    EXPECT_NE(error.find("EncoderDisconnect"), std::string::npos);
    EXPECT_NE(error.find("SensorRail"), std::string::npos);
    EXPECT_FALSE(RTESim::ParseEventSpec("t=1 kind=fault-inject arg=OpenGte", ev, error));
    // fault-clear semantics unchanged: free-form names still pass
    EXPECT_TRUE(RTESim::ParseEventSpec("t=1 kind=fault-clear arg=Whatever", ev, error)) << error;
    // F36: gate-ready is strictly 0|1
    EXPECT_FALSE(RTESim::ParseEventSpec("t=1 kind=gate-ready arg=yes", ev, error));
    EXPECT_FALSE(RTESim::ParseEventSpec("t=1 kind=gate-ready", ev, error));
    // plant-input needs Name=Value with a finite number
    EXPECT_FALSE(RTESim::ParseEventSpec("t=1 kind=plant-input arg=Omega_Cmd", ev, error));
    EXPECT_FALSE(RTESim::ParseEventSpec("t=1 kind=plant-input arg=Omega_Cmd=fast", ev, error));
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

    // F03/F11: python set_supply spellings, incl. the clamp + series-R keys
    RTESim::SupplyOverride clamp;
    ASSERT_TRUE(RTESim::ParseSupplySpec(
        "cv_clamp_v=150,cv_clamp_sink_a=20,series_r_mohm=7.5", clamp, error)) << error;
    EXPECT_NEAR(clamp.cvClamp, 150.0, 1e-12);
    EXPECT_NEAR(clamp.cvClampSink, 20.0, 1e-12);
    EXPECT_NEAR(clamp.seriesRmOhm, 7.5, 1e-12);
    EXPECT_TRUE(clamp.any());
}

TEST(SupplyParse, RejectsBadSpecs) {
    RTESim::SupplyOverride supply;
    std::string error;
    EXPECT_FALSE(RTESim::ParseSupplySpec("", supply, error));   // F28
    EXPECT_FALSE(RTESim::ParseSupplySpec("v_set=abc", supply, error));
    EXPECT_FALSE(RTESim::ParseSupplySpec("v_set=nan", supply, error));
    EXPECT_FALSE(RTESim::ParseSupplySpec("bogus=1", supply, error));
    EXPECT_FALSE(RTESim::ParseSupplySpec("can_sink=yes", supply, error));
    EXPECT_FALSE(RTESim::ParseSupplySpec("v_set", supply, error));
    EXPECT_FALSE(RTESim::ParseSupplySpec("i_limit_a=400", supply, error));  // old spelling
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
    // F15: provenance stamp appended last; replay ignores unknown keys
    EXPECT_EQ(RTESim::FormatSessionStartLine("sim_smoke", "rte-sim"),
              R"({"type":"session_start","format":"rte-runtime-session-jsonl","version":3,"t":0,"port":"rte-sim","mode":"sim","protocol":"legacy","scenario":"sim_smoke","plant":"generated"})");
    EXPECT_EQ(RTESim::FormatSessionStartLine("sim_smoke", "rte-sim", "fallback"),
              R"({"type":"session_start","format":"rte-runtime-session-jsonl","version":3,"t":0,"port":"rte-sim","mode":"sim","protocol":"legacy","scenario":"sim_smoke","plant":"fallback"})");
}

TEST(SessionSchema, EscapesQuotes) {
    EXPECT_EQ(RTESim::JStr("a\"b\\c"), "a\\\"b\\\\c");
}

TEST(SessionSchema, EscapesControlChars) {
    // F32: json.dumps parity — \n and \t shortcuts, other <0x20 as \u00XX
    EXPECT_EQ(RTESim::JStr("a\nb\tc"), "a\\nb\\tc");
    EXPECT_EQ(RTESim::JStr(std::string("x\ry\01z", 4)), "x\\u000dy\\u0001");
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

/* --------------------------------------------------------------------------
 * FRAM config seed derivation (graph-derived seed over the twin baseline)
 * -------------------------------------------------------------------------- */

namespace {

const RTESim::ConfigSeedEntry* FindSeed(const std::vector<RTESim::ConfigSeedEntry>& seed,
                                        const std::string& key) {
    for (const auto& e : seed) {
        if (e.key == key) return &e;
    }
    return nullptr;
}

} // namespace

TEST(ConfigSeed, BaselineIsTheLegacyTable) {
    const auto seed = RTESim::ConfigSeedBaseline();
    // the 12 keys today's hardcoded host literal pinned, in its order
    const std::vector<std::pair<std::string, double>> expected = {
        {"Motor.Encoder.SinCos.Sign", 1.0},
        {"Motor.Encoder.SinCos.OffsetDeg", 33.034},
        {"Motor.Poles", 10.0},
        {"Ctrl.PiQ.Kp", 0.03},
        {"Ctrl.PiQ.Ki", 10.0},
        {"Ctrl.PiD.Kp", 0.03},
        {"Ctrl.PiD.Ki", 10.0},
        {"Ctrl.Iq.SlewAps", 50.0},
        {"Ctrl.ThrottleEnable", 1.0},
        {"Motor.Ld", 100e-6},
        {"Motor.Lq", 100e-6},
        {"Motor.Lambda", 0.040},
    };
    ASSERT_EQ(seed.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(seed[i].key, expected[i].first);
        EXPECT_DOUBLE_EQ(seed[i].value, expected[i].second) << seed[i].key;
    }
}

TEST(ConfigSeed, GraphSilentOnConfigsKeepsBaseline) {
    // fallback case: no Values.Config nodes (and malformed input) must both
    // leave the baseline untouched — nothing twin-compatible may change.
    const auto empty_graph = RTESim::DeriveConfigSeed(R"({"nodes": []})");
    const auto baseline = RTESim::ConfigSeedBaseline();
    ASSERT_EQ(empty_graph.size(), baseline.size());
    for (size_t i = 0; i < baseline.size(); ++i) {
        EXPECT_EQ(empty_graph[i].key, baseline[i].key);
        EXPECT_DOUBLE_EQ(empty_graph[i].value, baseline[i].value);
    }
    // motor-graph-shaped input whose nodes are not config nodes
    const auto other = RTESim::DeriveConfigSeed(
        R"({"nodes":[{"id":"X","type":"Values.Var","parameters":{"Key":"Ctrl.PiQ.Kp","DefaultValue":"99"}}]})");
    EXPECT_DOUBLE_EQ(FindSeed(other, "Ctrl.PiQ.Kp")->value, 0.03);
    // unparseable JSON: conservatively baseline (the emitter errors separately)
    EXPECT_EQ(RTESim::DeriveConfigSeed("not json {").size(), baseline.size());
}

TEST(ConfigSeed, GraphOverridesBaselineKeysAndAppendsNew) {
    // override case: a graph's Values.Config DefaultValue must land in the
    // seed at the SAME slot (baseline keys keep order), and graph-only keys
    // append in graph order. Mirrors the CfgIqSlew dogfood edit.
    const auto seed = RTESim::DeriveConfigSeed(R"({
        "nodes": [
          {"id": "CfgIqSlew", "type": "Values.Config",
           "parameters": {"Cached": "0.0", "DefaultValue": "5.0", "Key": "Ctrl.Iq.SlewAps"}},
          {"id": "Trim1", "type": "config.trim",
           "parameters": {"Cached": "0.0", "DefaultValue": "7.5", "Key": "Ctrl.Trim"}},
          {"id": "Poles", "type": "Values.Config",
           "parameters": {"Cached": "0.0", "DefaultValue": "8.0", "Key": "Motor.Poles"}}
        ]})");
    const auto baseline = RTESim::ConfigSeedBaseline();
    ASSERT_EQ(seed.size(), baseline.size() + 1);
    EXPECT_DOUBLE_EQ(FindSeed(seed, "Ctrl.Iq.SlewAps")->value, 5.0);
    EXPECT_DOUBLE_EQ(FindSeed(seed, "Motor.Poles")->value, 8.0);
    // unmentioned keys keep baseline values
    EXPECT_DOUBLE_EQ(FindSeed(seed, "Ctrl.PiQ.Kp")->value, 0.03);
    EXPECT_DOUBLE_EQ(FindSeed(seed, "Motor.Lambda")->value, 0.040);
    // baseline slots keep their order; the graph-only key appended last
    for (size_t i = 0; i < baseline.size(); ++i) {
        EXPECT_EQ(seed[i].key, baseline[i].key);
    }
    EXPECT_EQ(seed.back().key, "Ctrl.Trim");
    EXPECT_DOUBLE_EQ(seed.back().value, 7.5);
}

TEST(ConfigSeed, IgnoresMalformedConfigNodes) {
    const auto seed = RTESim::DeriveConfigSeed(R"({
        "nodes": [
          {"id": "A", "type": "Values.Config",
           "parameters": {"Cached": "0.0", "DefaultValue": "9"}},
          {"id": "B", "type": "Values.Config",
           "parameters": {"Key": "", "DefaultValue": "9"}},
          {"id": "C", "type": "Values.Config",
           "parameters": {"Key": "Ctrl.Bad", "DefaultValue": "abc"}},
          {"id": "D", "type": "Values.Config",
           "parameters": {"Key": "Ctrl.NoDefault"}},
          {"id": "E", "type": "Values.Config",
           "parameters": {"Key": "Ctrl.Quoted\"Key", "DefaultValue": "9"}}
        ]})");
    const auto baseline = RTESim::ConfigSeedBaseline();
    ASSERT_EQ(seed.size(), baseline.size());
    EXPECT_EQ(FindSeed(seed, "Ctrl.Bad"), nullptr);
}

TEST(ConfigSeed, RenderIncludeShape) {
    const auto inc = RTESim::RenderConfigSeedInclude(RTESim::ConfigSeedBaseline());
    EXPECT_NE(inc.find("kGraphConfigSeed[]"), std::string::npos);
    EXPECT_NE(inc.find("{\"Ctrl.Iq.SlewAps\", 50.0f}"), std::string::npos);
    EXPECT_NE(inc.find("{\"Motor.Encoder.SinCos.OffsetDeg\", 33.034f}"), std::string::npos);
    EXPECT_NE(inc.find("{\"Motor.Ld\", 0.0001f}"), std::string::npos);
    EXPECT_NE(inc.find("std::pair<const char*, float>"), std::string::npos);
}

} // namespace
