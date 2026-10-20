#include "RTESim/Scenario.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace RTESim {

namespace {

bool ParseDouble(const std::string& text, double& value) {
    if (text.empty()) return false;
    char* end = nullptr;
    value = std::strtod(text.c_str(), &end);
    return end != text.c_str() && *end == '\0' && std::isfinite(value);
}

} // namespace

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
    static const char* kKinds[] = {"reset", "start", "var-set",
                                   "fault-inject", "fault-clear", "dyno-set"};
    bool known = false;
    for (const char* k : kKinds) known = known || event.kind == k;
    if (!known) {
        error = "unknown event kind: '" + event.kind +
                "' (reset|start|var-set|fault-inject|fault-clear|dyno-set)";
        return false;
    }
    if (event.kind == "var-set") {
        const std::size_t eq = arg.find('=');
        if (eq == std::string::npos || eq == 0) {
            error = "var-set needs arg=<Name>=<Value>";
            return false;
        }
        double parsed = 0.0;
        if (!ParseDouble(arg.substr(eq + 1), parsed)) {
            error = "var-set value is not a number: '" + arg.substr(eq + 1) + "'";
            return false;
        }
    }
    if (event.kind == "dyno-set") {
        double rpm = 0.0;
        if (!ParseDouble(arg, rpm)) {
            error = "dyno-set needs arg=<rpm>";
            return false;
        }
    }
    if (event.kind == "fault-inject" && arg.empty()) {
        error = "fault-inject needs arg=<OpenGates|EncoderDisconnect|SensorRail>";
        return false;
    }
    event.arg = arg;
    return true;
}

bool ParseSupplySpec(const std::string& spec, SupplyOverride& supply, std::string& error) {
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
            else {
                error = "unknown supply key: '" + key + "' (v_set|i_limit|sink_limit|can_sink)";
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
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            default: out += c;
        }
    }
    return out;
}

std::string FormatSessionStartLine(const std::string& scenario, const std::string& port) {
    std::string line = "{\"type\":\"session_start\",\"format\":\"rte-runtime-session-jsonl\","
                       "\"version\":3,\"t\":0,\"port\":\"";
    line += JStr(port);
    line += "\",\"mode\":\"sim\",\"protocol\":\"legacy\",\"scenario\":\"";
    line += JStr(scenario);
    line += "\"}";
    return line;
}

} // namespace RTESim
