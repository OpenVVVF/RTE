#pragma once

/**
 * Scenario vocabulary shared by the SimRunner (rte binary) and the compiled
 * scenario host binary. This header is compiled into BOTH programs, so it
 * must stay free of NodeAPI / RTE dependencies (std only, C++17).
 *
 * Record-format helpers (jnum/jstr/rt6 + FormatSessionStartLine) replicate
 * twin/native/src/main.cpp's SessionWriter byte-for-byte so the emitted
 * session loads through twin.replay / twin.metrics exactly like the twin
 * harness's v3 JSONL.
 */

#include <string>
#include <vector>

namespace RTESim {

/* Checked numeric parse shared by the rte CLI layer and the scenario host
 * (F21: full-consumption strtod + isfinite; rejects nan/inf at the seam). */
bool ParseDouble(const std::string& text, double& value);

/* --------------------------------------------------------------------------
 * Scenario events (CLI spelling: 't=<seconds> kind=<kind> arg=<payload>')
 * -------------------------------------------------------------------------- */

struct ScenarioEvent {
    double t = 0.0;
    std::string kind;   // reset | start | stop | var-set | plant-input |
                        // fault-inject | fault-clear | dyno-set | gate-ready
    std::string arg;    // var-set/plant-input: "Name=Value"; fault-*: fault
                        // name ("" = all); dyno-set: rpm as text;
                        // gate-ready: "0"|"1"
};

bool ParseEventSpec(const std::string& spec, ScenarioEvent& event, std::string& error);

// The fault-inject vocabulary (F01): output ports of Plant.Fault.Point.
// fault-clear accepts the same names plus the empty arg (= all).
extern const char* const kFaultNames[3];

/* --------------------------------------------------------------------------
 * Bench supply overrides (F03/F11 — python set_supply spellings).
 * CLI: 'v_set=..,i_limit=..,sink_limit=..,can_sink=0|1,cv_clamp_v=..,
 *       cv_clamp_sink_a=..,series_r_mohm=..'
 * Unset keys keep the graph's own DcBus values (plant::PlantSupplyDefaults).
 * -------------------------------------------------------------------------- */

struct SupplyOverride {
    bool hasVSet = false;
    bool hasILimit = false;
    bool hasSinkLimit = false;
    bool hasCanSink = false;
    bool hasCvClamp = false;
    bool hasCvClampSink = false;
    bool hasSeriesRmOhm = false;
    double vSet = 0.0;
    double iLimit = 0.0;
    double sinkLimit = 0.0;
    bool canSink = false;
    double cvClamp = 0.0;
    double cvClampSink = 0.0;
    double seriesRmOhm = 0.0;

    bool any() const {
        return hasVSet || hasILimit || hasSinkLimit || hasCanSink ||
               hasCvClamp || hasCvClampSink || hasSeriesRmOhm;
    }
};

bool ParseSupplySpec(const std::string& spec, SupplyOverride& supply, std::string& error);

/* --------------------------------------------------------------------------
 * Dyno speed series (CLI spelling: 't0=rpm0,t1=rpm1,...' piecewise-constant)
 * -------------------------------------------------------------------------- */

struct DynoStep {
    double t = 0.0;
    double rpm = 0.0;
};

bool ParseDynoSeries(const std::string& spec, std::vector<DynoStep>& series, std::string& error);

/* --------------------------------------------------------------------------
 * v3 session JSONL primitives (copied from twin/native/src/main.cpp)
 * -------------------------------------------------------------------------- */

std::string JNum(double v);                                    // %.9g, null on NaN/Inf
std::string JStr(const std::string& s);                        // JSON string escaping
double Rt6(double t);                                          // round to microseconds

// First line of every session file. Vocabulary pinned by twin/harness.py
// TwinSession.write_jsonl; the port for rte-sim runs is "rte-sim".
// `plant` stamps build provenance (F15: "generated"|"fallback"); it goes
// last and replay ignores unknown keys.
std::string FormatSessionStartLine(const std::string& scenario, const std::string& port,
                                   const std::string& plant = "generated");

} // namespace RTESim
