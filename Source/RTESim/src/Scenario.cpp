#include "RTESim/Scenario.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace RTESim {

const char* const kFaultNames[3] = {"OpenGates", "EncoderDisconnect", "SensorRail"};

bool ParseDouble(const std::string& text, double& value) {
    if (text.empty()) return false;
    char* end = nullptr;
    value = std::strtod(text.c_str(), &end);
    return end != text.c_str() && *end == '\0' && std::isfinite(value);
}

bool ParseEventSpec(const std::string& spec, ScenarioEvent& event, std::string& error) {
    // 't=.. kind=.. arg=..' (whitespace-separated key=value; arg may itself
    // contain '=' e.g. arg=IqVar=150, but no whitespace)
    bool haveT = false, haveKind = false;
    std::string arg;
    std::size_t pos = 0;
    while (pos < spec.size()) {
        const std::size_t end = spec.find(' ', pos);
        const std::string token = spec.substr(pos, end == std::string::npos ? end : end - pos);
        pos = end == std::string::npos ? spec.size() : end + 1;
        if (token.empty()) continue;
        const std::size_t eq = token.find('=');
        if (eq == std::string::npos) {
            error = "event token is not key=value: '" + token + "'";
            return false;
        }
        const std::string key = token.substr(0, eq);
        const std::string value = token.substr(eq + 1);
        if (key == "t") {
            if (!ParseDouble(value, event.t) || event.t < 0.0) {
                error = "event t is not a non-negative number: '" + value + "'";
                return false;
            }
            haveT = true;
        } else if (key == "kind") {
            event.kind = value;
            haveKind = true;
        } else if (key == "arg") {
            arg = value;
        } else {
            error = "unknown event key: '" + key + "'";
            return false;
        }
    }
    if (!haveT || !haveKind) {
        error = "event needs t=<seconds> and kind=<kind>";
        return false;
    }
    static const char* kKinds[] = {"reset", "start", "stop", "var-set", "plant-input",
                                   "fault-inject", "fault-clear", "dyno-set", "gate-ready"};
    bool known = false;
    for (const char* k : kKinds) known = known || event.kind == k;
    if (!known) {
        error = "unknown event kind: '" + event.kind +
                "' (reset|start|stop|var-set|plant-input|fault-inject|fault-clear|"
                "dyno-set|gate-ready)";
        return false;
    }
    auto parseNameValue = [&](const char* what) {
        const std::size_t eq = arg.find('=');
        if (eq == std::string::npos || eq == 0) {
            error = std::string(what) + " needs arg=<Name>=<Value>";
            return false;
        }
        double parsed = 0.0;
        if (!ParseDouble(arg.substr(eq + 1), parsed)) {
            error = std::string(what) + " value is not a number: '" + arg.substr(eq + 1) + "'";
            return false;
        }
        return true;
    };
    if (event.kind == "var-set" && !parseNameValue("var-set")) return false;
    // F13/F7: one-shot plant::PlantSetInput for controller-side externals
    // (InductDq.Omega_Cmd on the V/Hz rigs); same Name=Value grammar.
    if (event.kind == "plant-input" && !parseNameValue("plant-input")) return false;
    if (event.kind == "dyno-set") {
        double rpm = 0.0;
        if (!ParseDouble(arg, rpm)) {
            error = "dyno-set needs arg=<rpm>";
            return false;
        }
    }
    if (event.kind == "gate-ready") {
        // F36: mirrors the twin harness gate_ready vocabulary.
        if (arg != "0" && arg != "1") {
            error = "gate-ready needs arg=0|1, got '" + arg + "'";
            return false;
        }
    }
    if (event.kind == "fault-inject") {
        // F01: validate against the Plant.Fault.Point port names; a typo'd
        // fault name must die at the CLI, not "inject" nothing.
        // (fault-clear semantics unchanged: any name passes through.)
        bool match = false;
        for (const char* n : kFaultNames) match = match || arg == n;
        if (!match) {
            error = "unknown fault name: '" + arg +
                    "' (valid: OpenGates|EncoderDisconnect|SensorRail)";
            return false;
        }
    }
    event.arg = arg;
    return true;
}

bool ParseSupplySpec(const std::string& spec, SupplyOverride& supply, std::string& error) {
    // F28: an empty spec is a usage error (mirrors --dyno-series), and the
    // fill keys are the python set_supply spellings (F03/F11).
    if (spec.empty()) {
        error = "supply spec is empty";
        return false;
    }
    std::size_t pos = 0;
    while (pos <= spec.size()) {
        const std::size_t end = spec.find(',', pos);
        const std::string token = spec.substr(pos, end == std::string::npos ? end : end - pos);
        pos = end == std::string::npos ? spec.size() + 1 : end + 1;
        if (token.empty()) continue;
        const std::size_t eq = token.find('=');
        if (eq == std::string::npos) {
            error = "supply token is not key=value: '" + token + "'";
            return false;
        }
        const std::string key = token.substr(0, eq);
        const std::string value = token.substr(eq + 1);
        if (key == "can_sink") {
            if (value != "0" && value != "1") {
                error = "can_sink must be 0 or 1, got '" + value + "'";
                return false;
            }
            supply.canSink = value == "1";
            supply.hasCanSink = true;
        } else {
            double parsed = 0.0;
            if (!ParseDouble(value, parsed)) {
                error = "supply value is not a number: '" + token + "'";
                return false;
            }
            if (key == "v_set") { supply.vSet = parsed; supply.hasVSet = true; }
            else if (key == "i_limit") { supply.iLimit = parsed; supply.hasILimit = true; }
            else if (key == "sink_limit") { supply.sinkLimit = parsed; supply.hasSinkLimit = true; }
            else if (key == "cv_clamp_v") { supply.cvClamp = parsed; supply.hasCvClamp = true; }
            else if (key == "cv_clamp_sink_a") { supply.cvClampSink = parsed; supply.hasCvClampSink = true; }
            else if (key == "series_r_mohm") { supply.seriesRmOhm = parsed; supply.hasSeriesRmOhm = true; }
            else {
                error = "unknown supply key: '" + key +
                        "' (v_set|i_limit|sink_limit|can_sink|cv_clamp_v|"
                        "cv_clamp_sink_a|series_r_mohm)";
                return false;
            }
        }
    }
    return true;
}

bool ParseDynoSeries(const std::string& spec, std::vector<DynoStep>& series, std::string& error) {
    series.clear();
    std::size_t pos = 0;
    while (pos <= spec.size()) {
        const std::size_t end = spec.find(',', pos);
        const std::string token = spec.substr(pos, end == std::string::npos ? end : end - pos);
        pos = end == std::string::npos ? spec.size() + 1 : end + 1;
        if (token.empty()) continue;
        const std::size_t eq = token.find('=');
        if (eq == std::string::npos) {
            error = "dyno series token is not t=rpm: '" + token + "'";
            return false;
        }
        DynoStep step;
        if (!ParseDouble(token.substr(0, eq), step.t) || step.t < 0.0 ||
            !ParseDouble(token.substr(eq + 1), step.rpm)) {
            error = "dyno series token is not t=rpm: '" + token + "'";
            return false;
        }
        series.push_back(step);
    }
    if (series.empty()) {
        error = "dyno series is empty";
        return false;
    }
    return true;
}

/* --------------------------------------------------------------------------
 * v3 session JSONL primitives — byte-compatible copies of the helpers in
 * twin/native/src/main.cpp (jnum/rt6/jstr) so twin.replay accepts the files.
 * -------------------------------------------------------------------------- */

std::string JNum(double v) {
    if (!std::isfinite(v)) return "null";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.9g", v);
    return buf;
}

double Rt6(double t) { return std::round(t * 1e6) / 1e6; }

std::string JStr(const std::string& s) {
    // F32: JSON string escaping matching python json.dumps — control
    // characters (<0x20) as \u00XX, with \n / \t shortcuts.
    std::string out;
    out.reserve(s.size() + 2);
    char esc[8];
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    std::snprintf(esc, sizeof esc, "\\u%04x", c);
                    out += esc;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string FormatSessionStartLine(const std::string& scenario, const std::string& port,
                                   const std::string& plant) {
    std::string line = "{\"type\":\"session_start\",\"format\":\"rte-runtime-session-jsonl\","
                       "\"version\":3,\"t\":0,\"port\":\"";
    line += JStr(port);
    line += "\",\"mode\":\"sim\",\"protocol\":\"legacy\",\"scenario\":\"";
    line += JStr(scenario);
    line += "\",\"plant\":\"";
    line += JStr(plant);   // F15: build provenance; appended last (replay
                           // ignores unknown keys)
    line += "\"}";
    return line;
}

} // namespace RTESim
