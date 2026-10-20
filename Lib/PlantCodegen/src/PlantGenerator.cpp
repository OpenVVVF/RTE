#include "PlantCodegen/PlantGenerator.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace PlantCodegen {

namespace {

// ---------------------------------------------------------------------------
// C++/identifier utilities (token-level; template bodies follow the
// PlantTemplates ground rules: port/param names + DtPlant + plain math
// functions + M_PI/M_SQRT3 only).

const std::set<std::string>& Keywords() {
    static const std::set<std::string> k = {
        "alignas", "alignof", "and", "asm", "auto", "bool", "break", "case",
        "catch", "char", "class", "const", "constexpr", "continue", "decltype",
        "default", "delete", "do", "double", "else", "enum", "explicit",
        "export", "extern", "false", "float", "for", "friend", "goto", "if",
        "inline", "int", "long", "mutable", "namespace", "new", "noexcept",
        "nullptr", "operator", "private", "protected", "public", "register",
        "reinterpret_cast", "return", "short", "signed", "sizeof", "static",
        "static_cast", "struct", "switch", "template", "this", "throw", "true",
        "try", "typedef", "typeid", "typename", "union", "unsigned", "using",
        "virtual", "void", "volatile", "while",
    };
    return k;
}

bool IsIdentStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool IsIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool IsValidIdentifier(const std::string& name) {
    if (name.empty() || !IsIdentStart(name.front())) return false;
    for (char c : name) {
        if (!IsIdentChar(c)) return false;
    }
    return Keywords().count(name) == 0;
}

bool IsIdentifierToken(const std::string& tok) {
    return !tok.empty() && IsIdentStart(tok.front()) && Keywords().count(tok) == 0;
}

// Removes /* */ and // comments plus string/char literals (replaced by
// spaces so token boundaries survive) before identifier scans.
std::string StripCode(const std::string& code) {
    std::string out = code;
    const std::size_t n = code.size();
    std::size_t i = 0;
    while (i < n) {
        if (code.compare(i, 2, "/*") == 0) {
            const std::size_t end = code.find("*/", i + 2);
            const std::size_t stop = end == std::string::npos ? n : end + 2;
            for (std::size_t j = i; j < stop; ++j) {
                if (out[j] != '\n') out[j] = ' ';
            }
            i = stop;
        } else if (code.compare(i, 2, "//") == 0) {
            std::size_t end = code.find('\n', i);
            if (end == std::string::npos) end = n;
            for (std::size_t j = i; j < end; ++j) out[j] = ' ';
            i = end;
        } else if (code[i] == '"' || code[i] == '\'') {
            const char quote = code[i];
            std::size_t j = i + 1;
            while (j < n && code[j] != quote) {
                if (code[j] == '\\') ++j;
                ++j;
            }
            for (std::size_t k = i; k <= j && k < n; ++k) out[k] = ' ';
            i = j < n ? j + 1 : n;
        } else {
            ++i;
        }
    }
    return out;
}

std::vector<std::string> Tokenize(const std::string& code) {
    static const std::set<std::string> kTwo = {
        "==", "!=", "<=", ">=", "+=", "-=", "*=", "/=", "%=", "++", "--",
        "->", "&&", "||", "<<", ">>",
    };
    std::vector<std::string> tokens;
    const std::string clean = StripCode(code);
    std::size_t i = 0;
    while (i < clean.size()) {
        const char c = clean[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
        } else if (IsIdentStart(c) || std::isdigit(static_cast<unsigned char>(c))) {
            std::size_t j = i;
            while (j < clean.size() &&
                   (IsIdentChar(clean[j]) || clean[j] == '.')) {
                ++j;
            }
            tokens.push_back(clean.substr(i, j - i));
            i = j;
        } else {
            const std::string two = clean.substr(i, 2);
            if (i + 1 < clean.size() && kTwo.count(two)) {
                tokens.push_back(two);
                i += 2;
            } else {
                tokens.push_back(std::string(1, c));
                ++i;
            }
        }
    }
    return tokens;
}

const std::set<std::string>& TypeWords() {
    static const std::set<std::string> k = {
        "double", "float", "int", "long", "short", "unsigned", "signed",
        "char", "bool", "auto", "size_t",
    };
    return k;
}

// Identifiers declared inside the token stream (type-led declarations).
void AddDeclared(const std::vector<std::string>& tokens, std::set<std::string>& out) {
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        std::size_t t = i;
        if (tokens[t] == "const" && t + 1 < tokens.size()) ++t;
        if (!TypeWords().count(tokens[t])) continue;
        std::size_t j = t + 1;
        while (j < tokens.size() && (tokens[j] == "*" || tokens[j] == "&")) ++j;
        while (j < tokens.size() && IsIdentifierToken(tokens[j])) {
            out.insert(tokens[j]);
            if (j + 1 < tokens.size() && tokens[j + 1] == ",") {
                j += 2;
            } else {
                break;
            }
        }
    }
}

std::set<std::string> DeclaredLocals(const std::string& code) {
    std::set<std::string> out;
    AddDeclared(Tokenize(code), out);
    return out;
}

std::set<std::string> UsedIdentifiers(const std::string& code) {
    std::set<std::string> out;
    for (const auto& tok : Tokenize(code)) {
        if (IsIdentifierToken(tok)) out.insert(tok);
    }
    return out;
}

// Identifiers that appear on the left of = / += / -= / *= / /= / %= / ++ / --,
// minus identifiers declared locally in the same body.
std::set<std::string> AssignedIdentifiers(const std::string& code) {
    static const std::set<std::string> kCompound = {
        "+=", "-=", "*=", "/=", "%=", "++", "--",
    };
    std::set<std::string> out;
    const auto tokens = Tokenize(code);
    for (std::size_t i = 1; i < tokens.size(); ++i) {
        if ((tokens[i] == "=" || kCompound.count(tokens[i])) &&
            (i >= 1 && IsIdentifierToken(tokens[i - 1]))) {
            out.insert(tokens[i - 1]);
        }
    }
    std::set<std::string> locals;
    AddDeclared(tokens, locals);
    for (const auto& local : locals) out.erase(local);
    return out;
}

// ---------------------------------------------------------------------------
// Node classification (PlantTemplates/README.md execution contract).

enum class NodeClass { kHost, kStateful, kCombinational };

std::pair<NodeClass, bool> Classify(const std::string& typeId) {
    static const std::set<std::string> kHost = {
        "Plant.Io.DutyIn", "Plant.Fault.Point", "Plant.Shaft.Dyno",
    };
    static const std::set<std::string> kStateful = {
        "Plant.Machine.PmsmDq", "Plant.Machine.InductDq",
        "Plant.Shaft.FreeInertia", "Plant.DcBus",
        "Plant.Thermal.TwoNodeCauer", "Plant.Sensor.VoltageSense",
    };
    if (kHost.count(typeId)) return {NodeClass::kHost, true};
    if (kStateful.count(typeId)) return {NodeClass::kStateful, true};
    // Everything else (MeasOut, inverter/loss, sensors, utilities) is
    // combinational per the README contract.
    static const std::set<std::string> kComb = {
        "Plant.Io.MeasOut", "Plant.Inverter.Knee3ph", "Plant.Loss.FittedLaw",
        "Plant.Sensor.Hall", "Plant.Sensor.SinCosEncoder", "Plant.Util.Sum",
        "Plant.Util.Hypot", "Plant.Util.AbsOmega",
    };
    return {NodeClass::kCombinational, kComb.count(typeId) > 0};
}

// Parameter defaults mirroring the python reference's Node-class fallbacks
// (twin/plantgraph/plantgraph.py `self.p.get(name, default)`), applied when
// the graph instance omits a parameter a pasted body references.
std::optional<double> TemplateDefault(const std::string& typeId,
                                      const std::string& param) {
    static const std::map<std::pair<std::string, std::string>, double> kDefaults = {
        {{"Plant.Loss.FittedLaw", "IdleHeat_W"}, 120.0},      // .get(...,120.0)
        {{"Plant.DcBus", "V0_V"}, 0.0},                        // .get(...,0.0)
        {{"Plant.DcBus", "CanSink"}, 0.0},                     // .get(...,0.0)
        {{"Plant.DcBus", "SinkLimit_A"}, 1e9},                 // .get(...,1e9)
        {{"Plant.DcBus", "CvClamp_V"}, 0.0},                   // .get(...,0.0)
        {{"Plant.DcBus", "CvClampSink_A"}, 0.0},               // clamp-branch default
        {{"Plant.Shaft.Dyno", "StartRpm"}, 0.0},               // .get(...,0.0)
        {{"Plant.Shaft.FreeInertia", "Rpm0"}, 0.0},            // .get(...,0.0)
        {{"Plant.Machine.PmsmDq", "Id0"}, 0.0},                // .get(...,0.0)
        {{"Plant.Machine.PmsmDq", "Iq0"}, 0.0},                // .get(...,0.0)
        {{"Plant.Machine.PmsmDq", "Theta0"}, 0.0},             // .get(...,0.0)
        {{"Plant.Sensor.VoltageSense", "LagTau_S"}, 0.0},      // .get(...,0.0)
    };
    const auto it = kDefaults.find({typeId, param});
    if (it == kDefaults.end()) return std::nullopt;
    return it->second;
}

// DcBus bench-supply parameters the host may re-point at runtime via
// PlantSetSupply: stored as mutable state fields, not constexpr.
bool IsSupplyParameter(const std::string& typeId, const std::string& param) {
    if (typeId != "Plant.DcBus") return false;
    return param == "VSet_V" || param == "ILimit_A" ||
           param == "SinkLimit_A" || param == "CanSink";
}

// Parses a graph parameter string as a full-consumption double.
bool ParseDouble(const std::string& text, double& out) {
    const std::size_t b = text.find_first_not_of(" \t\n\r");
    if (b == std::string::npos) return false;
    const std::size_t e = text.find_last_not_of(" \t\n\r");
    const std::string trimmed = text.substr(b, e - b + 1);
    char* end = nullptr;
    out = std::strtod(trimmed.c_str(), &end);
    return end != trimmed.c_str() && end != nullptr && *end == '\0';
}

std::string FormatDouble(double value) {
    std::ostringstream out;
    out << std::setprecision(17) << value;
    return out.str();
}

// ---------------------------------------------------------------------------
// Per-node analysis / validation.

struct NodeAnalysis {
    const NodeAPI::Node* node = nullptr;
    const NodeAPI::NodeType* type = nullptr;  // null when the type is unknown
    NodeClass cls = NodeClass::kCombinational;
    bool typeInTable = true;  // present in the classify tables

    std::vector<std::string> inputPorts;
    std::vector<std::string> outputPorts;

    std::map<std::string, NodeAPI::PortRef> wiredInputs;  // port -> producer
    std::vector<std::string> unwiredInputs;               // subset of inputPorts

    std::vector<std::string> internalState;  // persistent non-port vars (code order)
    std::vector<std::string> supplyParams;   // DcBus host-mutable params

    std::set<std::string> usedCtor;
    std::set<std::string> usedStep;
    std::string ctorBody;
    std::string stepBody;

    // name -> C++ literal text for every parameter a pasted body references
    // (graph value when present, python-parity default otherwise)
    std::map<std::string, std::string> paramLiterals;
    // parseable instance parameters -> literal (for the struct constexprs)
    std::map<std::string, std::string> staticParams;
};

struct Analysis {
    const NodeAPI::Graph& graph;
    std::vector<NodeAnalysis> nodes;  // declaration order
    std::map<std::string, std::size_t> indexById;
    std::vector<std::string> warnings;
};

Analysis Analyze(const NodeAPI::Graph& graph) {
    Analysis a{graph};
    std::map<std::string, const NodeAPI::NodeType*> typeById;
    for (const auto& t : graph.GetNodeTypes()) typeById.emplace(t.id, &t);

    for (const auto& node : graph.GetNodes()) {
        NodeAnalysis na;
        na.node = &node;
        const auto it = typeById.find(node.type);
        if (it != typeById.end()) na.type = it->second;
        const auto [cls, inTable] = Classify(node.type);
        na.cls = cls;
        na.typeInTable = inTable;

        if (na.type) {
            for (const auto& p : na.type->inputPorts) na.inputPorts.push_back(p.name);
            for (const auto& p : na.type->outputPorts) na.outputPorts.push_back(p.name);
            na.ctorBody = na.type->constructorCode;
            na.stepBody = na.type->inlineCode;
        }
        na.usedCtor = UsedIdentifiers(na.ctorBody);
        na.usedStep = UsedIdentifiers(na.stepBody);

        const std::string both = na.ctorBody + "\n" + na.stepBody;
        const auto assigned = AssignedIdentifiers(both);
        const auto locals = DeclaredLocals(both);

        // Parseable instance parameters (for struct constexprs).
        double value = 0.0;
        for (const auto& [key, text] : node.parameters) {
            if (ParseDouble(text, value) && !IsSupplyParameter(node.type, key)) {
                na.staticParams.emplace(key, text);
            }
        }

        // Internal persistent state: assigned, not local, not port, not param
        // (in first-appearance code order for a stable struct layout).
        const std::set<std::string> inputs(na.inputPorts.begin(), na.inputPorts.end());
        const std::set<std::string> outputs(na.outputPorts.begin(), na.outputPorts.end());
        for (const auto& tok : Tokenize(both)) {
            if (!assigned.count(tok)) continue;
            if (inputs.count(tok) || outputs.count(tok)) continue;
            if (locals.count(tok)) continue;
            if (na.staticParams.count(tok) || node.parameters.count(tok)) continue;
            if (std::find(na.internalState.begin(), na.internalState.end(), tok) ==
                na.internalState.end()) {
                na.internalState.push_back(tok);
            }
        }

        // Wiring map + unwired inputs.
        for (const auto& c : graph.GetConnections()) {
            if (c.to.nodeId != node.id) continue;
            na.wiredInputs.emplace(c.to.portName,
                                   NodeAPI::PortRef{c.from.nodeId, c.from.portName});
        }
        for (const auto& name : na.inputPorts) {
            if (!na.wiredInputs.count(name)) na.unwiredInputs.push_back(name);
        }

        // DcBus host-mutable supply parameters (always materialized so
        // PlantSetSupply reaches them even when the graph omits one).
        if (node.type == "Plant.DcBus") {
            na.supplyParams = {"VSet_V", "ILimit_A", "SinkLimit_A", "CanSink"};
        }

        a.indexById.emplace(node.id, a.nodes.size());
        a.nodes.push_back(std::move(na));
    }
    return a;
}

bool Validate(Analysis& a, std::string& error) {
    const NodeAPI::Graph& graph = a.graph;

    if (!graph.GetBridges().empty()) {
        error = "plant graphs disallow bridges (" +
                std::to_string(graph.GetBridges().size()) + " present)";
        return false;
    }

    std::set<std::string> seenIds;
    for (const auto& node : graph.GetNodes()) {
        if (!seenIds.insert(node.id).second) {
            error = "duplicate node id '" + node.id + "'";
            return false;
        }
        if (!IsValidIdentifier(node.id)) {
            error = "node id is not a valid C++ identifier: '" + node.id + "'";
            return false;
        }
        if (node.domain != "plant") {
            error = "node '" + node.id + "' has domain '" + node.domain +
                    "'; plant graphs use the single domain 'plant'";
            return false;
        }
    }

    for (const auto& na : a.nodes) {
        if (!na.type) {
            error = "node '" + na.node->id + "' has unknown type '" +
                    na.node->type + "' (not among the loaded node types)";
            return false;
        }
        if (!na.typeInTable) {
            a.warnings.push_back("node '" + na.node->id + "' has unlisted type '" +
                                 na.node->type + "'; scheduled as combinational");
        }
    }

    // maxInstances overruns (e.g. the maxInstances=1 reserved nodes
    // Plant.Io.DutyIn / Plant.Io.MeasOut).
    for (const auto& type : graph.GetNodeTypes()) {
        if (type.maxInstances == 0) continue;
        std::size_t count = 0;
        for (const auto& node : graph.GetNodes()) {
            if (node.type == type.id) ++count;
        }
        if (count > type.maxInstances) {
            error = "node type '" + type.id + "' allows at most " +
                    std::to_string(type.maxInstances) + " instance(s); the graph has " +
                    std::to_string(count);
            return false;
        }
    }

    // Connections: endpoints and ports exist; no input wired twice.
    std::map<std::string, const NodeAnalysis*> byId;
    for (const auto& na : a.nodes) byId.emplace(na.node->id, &na);
    std::set<NodeAPI::PortRef> wiredDst;
    for (const auto& c : graph.GetConnections()) {
        const auto fromIt = byId.find(c.from.nodeId);
        if (fromIt == byId.end()) {
            error = "connection '" + c.id + "' references unknown node '" +
                    c.from.nodeId + "'";
            return false;
        }
        const auto toIt = byId.find(c.to.nodeId);
        if (toIt == byId.end()) {
            error = "connection '" + c.id + "' references unknown node '" +
                    c.to.nodeId + "'";
            return false;
        }
        if (!fromIt->second->type->FindOutputPort(c.from.portName)) {
            error = "connection '" + c.id + "': node '" + c.from.nodeId +
                    "' has no output port '" + c.from.portName + "'";
            return false;
        }
        if (!toIt->second->type->FindInputPort(c.to.portName)) {
            error = "connection '" + c.id + "': node '" + c.to.nodeId +
                    "' has no input port '" + c.to.portName + "'";
            return false;
        }
        if (!wiredDst.insert(NodeAPI::PortRef{c.to.nodeId, c.to.portName}).second) {
            error = "input '" + c.to.nodeId + "." + c.to.portName +
                    "' is wired more than once";
            return false;
        }
    }

    // Unconnected required inputs (host nodes are driven by PlantSet* helpers;
    // MeasOut inputs are bound through the PlantMeas seam).
    for (const auto& na : a.nodes) {
        if (na.cls == NodeClass::kHost || na.node->type == "Plant.Io.MeasOut") continue;
        std::set<std::string> optionalInputs;
        for (const auto& p : na.type->inputPorts) {
            if (p.optional) optionalInputs.insert(p.name);
        }
        for (const auto& name : na.inputPorts) {
            if (!na.wiredInputs.count(name) && !optionalInputs.count(name)) {
                error = "input port '" + name + "' of node '" + na.node->id +
                        "' is not connected";
                return false;
            }
        }
    }

    // Parameters referenced by pasted bodies: graph values must parse;
    // missing values need a python-parity template default.
    for (auto& na : a.nodes) {
        const auto resolve = [&](const std::string& used,
                                 std::string& localError) -> bool {
            const auto instIt = na.node->parameters.find(used);
            const bool knownParam = instIt != na.node->parameters.end() ||
                                    na.type->FindParameterType(used).has_value() ||
                                    IsSupplyParameter(na.node->type, used);
            if (!knownParam) return true;  // ports/state/locals/functions
            if (instIt != na.node->parameters.end()) {
                double parsed = 0.0;
                if (!ParseDouble(instIt->second, parsed)) {
                    localError = "unparseable float parameter '" + used +
                                 "' on node '" + na.node->id + "': \"" +
                                 instIt->second + "\"";
                    return false;
                }
                na.paramLiterals.emplace(used, instIt->second);
                return true;
            }
            const auto fallback = TemplateDefault(na.node->type, used);
            if (!fallback) {
                localError = "node '" + na.node->id + "' body references parameter '" +
                             used + "' but the graph provides no value and the template"
                             " has no default";
                return false;
            }
            na.paramLiterals.emplace(used, FormatDouble(*fallback));
            return true;
        };
        for (const auto& used : na.usedCtor) {
            if (!resolve(used, error)) return false;
        }
        for (const auto& used : na.usedStep) {
            if (!resolve(used, error)) return false;
        }
    }

    // Struct-layout collisions (defensive; no current template mixes these).
    for (const auto& na : a.nodes) {
        std::set<std::string> fields(na.outputPorts.begin(), na.outputPorts.end());
        for (const auto& s : na.internalState) fields.insert(s);
        for (const auto& in : na.unwiredInputs) {
            if (na.node->type == "Plant.Io.MeasOut") continue;
            if (!fields.insert(in).second) {
                error = "node '" + na.node->id + "' has unwired input '" + in +
                        "' colliding with a state field";
                return false;
            }
        }
        for (const auto& s : na.supplyParams) {
            if (!fields.insert(s).second) {
                error = "supply parameter '" + s + "' of node '" + na.node->id +
                        "' collides with a state field";
                return false;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Scheduler: mirrors twin/plantgraph/plantgraph.py PlantGraphSim._run_pass
// exactly (offline, frozen at codegen time).

// Computes the frozen substep evaluation order (host nodes excluded; they
// emit no statements in PlantStep). Returns false with `error` on an
// unschedulable combinational cycle (the force-candidate rule can never
// otherwise stall).
bool ComputeSchedule(const Analysis& a, std::vector<std::string>& order,
                     std::string& error) {
    // Reverse-BFS ancestor cones over connections; stateful/host nodes are
    // opaque endpoints (only combinational nodes are traversed through).
    std::map<std::string, std::set<std::string>> inWires;  // dst -> src ids
    for (const auto& c : a.graph.GetConnections()) {
        inWires[c.to.nodeId].insert(c.from.nodeId);
    }

    const auto classOf = [&](const std::string& id) {
        return a.nodes.at(a.indexById.at(id)).cls;
    };

    std::map<std::string, std::set<std::string>> combAnc, stateAnc;
    for (const auto& na : a.nodes) {
        std::set<std::string> seen, comb, state;
        std::vector<std::string> stack(inWires[na.node->id].begin(),
                                       inWires[na.node->id].end());
        while (!stack.empty()) {
            const std::string s = stack.back();
            stack.pop_back();
            if (!seen.insert(s).second) continue;
            const NodeClass cls = classOf(s);
            if (cls == NodeClass::kStateful) {
                state.insert(s);
            } else if (cls == NodeClass::kCombinational) {
                comb.insert(s);
                const auto more = inWires[s];
                stack.insert(stack.end(), more.begin(), more.end());
            }
            // host nodes: opaque, never traversed
        }
        combAnc[na.node->id] = std::move(comb);
        stateAnc[na.node->id] = std::move(state);
    }

    // Fold transitive stateful ancestry through combinational ancestors.
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& na : a.nodes) {
            auto grow = stateAnc[na.node->id];
            for (const auto& c : combAnc[na.node->id]) {
                grow.insert(stateAnc[c].begin(), stateAnc[c].end());
            }
            if (grow != stateAnc[na.node->id]) {
                stateAnc[na.node->id] = std::move(grow);
                changed = true;
            }
        }
    }

    std::vector<std::string> hosts, combs, statefuls;
    for (const auto& na : a.nodes) {
        if (na.cls == NodeClass::kHost) hosts.push_back(na.node->id);
        else if (na.cls == NodeClass::kStateful) statefuls.push_back(na.node->id);
        else combs.push_back(na.node->id);
    }

    std::set<std::string> evaluated(hosts.begin(), hosts.end());
    std::set<std::string> integrated;
    std::vector<std::string> pendingState = statefuls;

    const auto ready = [&](const std::string& id) {
        for (const auto& c : combAnc[id]) {
            if (!evaluated.count(c)) return false;
        }
        for (const auto& s : stateAnc[id]) {
            if (!integrated.count(s)) return false;
        }
        return true;
    };

    const auto drain = [&]() {
        bool progressed = false;
        for (const auto& id : combs) {
            if (!evaluated.count(id) && ready(id)) {
                evaluated.insert(id);
                order.push_back(id);
                progressed = true;
            }
        }
        return progressed;
    };

    for (;;) {
        const bool progressed = drain();
        const auto readyIt = std::find_if(pendingState.begin(), pendingState.end(), ready);
        if (readyIt != pendingState.end()) {
            const std::string id = *readyIt;
            evaluated.insert(id);
            integrated.insert(id);
            order.push_back(id);
            pendingState.erase(readyIt);
            continue;
        }
        if (pendingState.empty()) {
            if (drain()) continue;
            std::vector<std::string> rest;
            for (const auto& id : combs) {
                if (!evaluated.count(id)) rest.push_back(id);
            }
            if (!rest.empty()) {
                error = "unschedulable combinational nodes (combinational cycle): ";
                for (const auto& id : rest) error += id + " ";
                return false;
            }
            break;
        }
        if (progressed) continue;

        // Cycle-locked: force the pending stateful with the fewest pending
        // stateful ancestors (declaration order tie-break); its pending
        // combinational ancestors evaluate first against PRE values.
        std::string cand;
        std::size_t bestCount = static_cast<std::size_t>(-1);
        std::size_t bestIndex = a.nodes.size();
        for (const auto& id : pendingState) {
            std::size_t pendingAncestors = 0;
            for (const auto& s : stateAnc[id]) {
                if (s != id && !integrated.count(s)) ++pendingAncestors;
            }
            const std::size_t declIndex = a.indexById.at(id);
            if (pendingAncestors < bestCount ||
                (pendingAncestors == bestCount && declIndex < bestIndex)) {
                cand = id;
                bestCount = pendingAncestors;
                bestIndex = declIndex;
            }
        }
        if (cand.empty()) {
            error = "scheduler stalled with pending stateful nodes and no force candidate";
            return false;
        }
        for (const auto& id : combs) {
            if (!evaluated.count(id) && combAnc[cand].count(id)) {
                evaluated.insert(id);
                order.push_back(id);
            }
        }
        evaluated.insert(cand);
        integrated.insert(cand);
        order.push_back(cand);
        pendingState.erase(std::find(pendingState.begin(), pendingState.end(), cand));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Emission.

std::string NodeTypeName(const NodeAnalysis& a) {
    return a.node->id + "Node";
}

// Binds the parameters a pasted body uses (constexpr locals for static
// values; const& for the host-mutable DcBus supply fields).
void EmitParameterBinds(std::ostringstream& out, const NodeAnalysis& a,
                        const std::set<std::string>& used, const char* indent) {
    for (const auto& [name, literal] : a.paramLiterals) {
        if (!used.count(name)) continue;
        const bool isSupply =
            std::find(a.supplyParams.begin(), a.supplyParams.end(), name) !=
            a.supplyParams.end();
        if (isSupply) {
            out << indent << "const double& " << name << " = state." << a.node->id
                << "." << name << ";\n";
        } else {
            out << indent << "constexpr double " << name << " = " << literal << ";\n";
        }
    }
}

void EmitInputBinds(std::ostringstream& out, const NodeAnalysis& a,
                    const std::set<std::string>& used, const char* indent) {
    for (const auto& name : a.inputPorts) {
        if (!used.count(name)) continue;
        const auto wire = a.wiredInputs.find(name);
        if (wire != a.wiredInputs.end()) {
            out << indent << "const float " << name << " = state." << wire->second.nodeId
                << "." << wire->second.portName << ";\n";
        } else {
            // Unwired input: zero-bound by default; PlantSetInput target.
            out << indent << "const float " << name << " = state." << a.node->id << "."
                << name << ";\n";
        }
    }
}

void EmitStateBinds(std::ostringstream& out, const NodeAnalysis& a,
                    const std::set<std::string>& used, const char* indent) {
    for (const auto& name : a.outputPorts) {
        if (!used.count(name)) continue;
        out << indent << "float& " << name << " = state." << a.node->id << "." << name
            << ";\n";
    }
    for (const auto& name : a.internalState) {
        if (!used.count(name)) continue;
        out << indent << "float& " << name << " = state." << a.node->id << "." << name
            << ";\n";
    }
}

std::string EmitBodyBlock(const NodeAnalysis& a, const char* indent) {
    std::ostringstream out;
    out << indent << "{\n";
    const std::string inner = std::string(indent) + "    ";
    out << inner << "[[maybe_unused]] const double DtPlant = (double)dt;\n";
    EmitInputBinds(out, a, a.usedStep, inner.c_str());
    EmitParameterBinds(out, a, a.usedStep, inner.c_str());
    EmitStateBinds(out, a, a.usedStep, inner.c_str());
    out << a.stepBody;
    if (!a.stepBody.empty() && a.stepBody.back() != '\n') out << "\n";
    out << indent << "}\n";
    return out.str();
}

// ---------------------------------------------------------------------------

bool WriteFile(const std::filesystem::path& path, const std::string& text,
               std::string& error) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "could not open " + path.string() + " for writing";
        return false;
    }
    file << text;
    if (!file.good()) {
        error = "failed writing " + path.string();
        return false;
    }
    return true;
}

std::string BuildHeader(const Analysis& a) {
    std::ostringstream out;
    out << "#pragma once\n\n";
    out << "// Generated by PlantCodegenLib from the plant nodegraph \""
        << a.graph.GetName() << "\"; do not edit by hand.\n";
    out << "// Execution contract: Assets/PlantTemplates/README.md; frozen\n";
    out << "// substep schedule mirrors twin/plantgraph/plantgraph.py.\n\n";

    out << "namespace plant {\n\n";

    out << "struct PlantState {\n";
    for (const auto& na : a.nodes) {
        out << "    struct " << NodeTypeName(na) << " {\n";
        bool wroteSection = false;
        const auto section = [&](const char* label, const auto& names,
                                 const char* typeName) {
            if (names.empty()) return;
            out << "        // " << label << "\n";
            for (const auto& name : names) {
                out << "        " << typeName << " " << name
                    << (std::string(typeName) == "double" ? " = 0.0" : " = 0.0f")
                    << ";\n";
            }
            wroteSection = true;
        };
        section("output ports", na.outputPorts, "float");
        section("internal persistent state", na.internalState, "float");
        if (na.node->type != "Plant.Io.MeasOut") {
            section("unwired inputs (zero-bound; PlantSetInput targets)",
                    na.unwiredInputs, "float");
        }
        section("host-adjustable supply parameters (PlantSetSupply)",
                na.supplyParams, "double");
        if (!na.staticParams.empty()) {
            out << "        // parameters (graph values)\n";
            for (const auto& [name, literal] : na.staticParams) {
                out << "        static constexpr double " << name << " = " << literal
                    << ";\n";
            }
            wroteSection = true;
        }
        if (!wroteSection) out << "        // (no ports/state)\n";
        out << "    } " << na.node->id << ";  // " << na.node->type << "\n";
    }
    out << "};\n\n";

    out << "void PlantInit(PlantState& state);\n";
    out << "void PlantStep(PlantState& state, double dt);\n\n";

    // PlantMeas: one field per Plant.Io.MeasOut template input port.
    std::vector<std::string> measInputs;
    for (const auto& type : a.graph.GetNodeTypes()) {
        if (type.id != "Plant.Io.MeasOut") continue;
        for (const auto& p : type.inputPorts) measInputs.push_back(p.name);
    }
    out << "struct PlantMeas {\n";
    for (const auto& name : measInputs) {
        out << "    float " << name << " = 0.0f;\n";
    }
    if (measInputs.empty()) out << "    // (no Plant.Io.MeasOut type loaded)\n";
    out << "};\n";
    out << "void PlantCollectMeas(const PlantState& state, PlantMeas& out);\n\n";

    out << "// Host-side externals (safe no-ops when the graph lacks the node;\n";
    out << "// PlantSetInput returns false when no unwired port matches).\n";
    out << "void PlantSetDuty(PlantState& state, float du, float dv, float dw);\n";
    out << "void PlantSetDynoRpm(PlantState& state, float rpm);\n";
    out << "void PlantSetFault(PlantState& state, const char* name, bool active);\n";
    out << "bool PlantSetInput(PlantState& state, const char* portName, float value);\n";
    out << "void PlantSetSupply(PlantState& state, double vSet, double iLimit,\n"
           "                      double sinkLimit, bool canSink);\n\n";

    out << "}  // namespace plant\n";
    return out.str();
}

std::string BuildSource(const Analysis& a, const std::vector<std::string>& order) {
    std::ostringstream out;
    const auto byId = [&](const std::string& id) -> const NodeAnalysis& {
        return a.nodes.at(a.indexById.at(id));
    };

    out << "// Generated by PlantCodegenLib from the plant nodegraph \""
        << a.graph.GetName() << "\"; do not edit by hand.\n";
    for (const auto& warning : a.warnings) {
        out << "// WARNING: " << warning << "\n";
    }
    out << "//\n// Frozen substep schedule (PlantGraphSim._run_pass semantics):\n";
    out << "//   host emit:";
    for (const auto& na : a.nodes) {
        if (na.cls == NodeClass::kHost) out << " " << na.node->id;
    }
    out << "\n//   bodies:";
    for (const auto& id : order) out << " " << id;
    out << "\n\n";

    out << "#include \"plant_generated.h\"\n\n";
    out << "#include <math.h>\n";
    out << "#include <cmath>\n";
    out << "#include <cstring>\n";
    out << "#include <limits>\n\n";
    out << "#ifndef M_PI\n#define M_PI 3.14159265358979323846\n#endif\n";
    out << "#ifndef M_SQRT3\n#define M_SQRT3 1.7320508075688772935\n#endif\n\n";

    out << "namespace plant {\n\n";

    // ----- PlantInit -----
    out << "void PlantInit(PlantState& state) {\n";
    out << "    state = PlantState{};\n";
    for (const auto& na : a.nodes) {
        out << "    // " << na.node->id << " (" << na.node->type << ")\n";

        // Host-mutable supply parameters first (graph values or defaults).
        for (const auto& name : na.supplyParams) {
            const auto lit = [&] {
                const auto it = na.node->parameters.find(name);
                if (it != na.node->parameters.end()) {
                    double parsed = 0.0;
                    if (ParseDouble(it->second, parsed)) return it->second;
                }
                return FormatDouble(TemplateDefault(na.node->type, name).value_or(0.0));
            }();
            out << "    state." << na.node->id << "." << name << " = " << lit << ";\n";
        }

        const bool needsScope = !na.ctorBody.empty() ||
                                na.node->type == "Plant.Shaft.Dyno";
        if (needsScope) {
            out << "    {\n";
            // Constructor: bind constructor-used params/state, paste verbatim.
            std::set<std::string> used = na.usedCtor;
            if (na.node->type == "Plant.Shaft.Dyno") {
                // Host publish (python _emit_hosts at initial eval): the dyno
                // body converts the assigned Rpm to electrical views.
                used.insert(na.usedStep.begin(), na.usedStep.end());
            }
            EmitParameterBinds(out, na, used, "        ");
            EmitStateBinds(out, na, used, "        ");
            out << na.ctorBody;
            if (!na.ctorBody.empty() && na.ctorBody.back() != '\n') out << "\n";
            if (na.node->type == "Plant.Shaft.Dyno") {
                out << na.stepBody;
                if (!na.stepBody.empty() && na.stepBody.back() != '\n') out << "\n";
            }
            out << "    }\n";
        } else if (na.node->type == "Plant.Io.DutyIn") {
            // python DutyIn.init: duty tuple defaults to 50 % (zero vector).
            out << "    state." << na.node->id << ".Du = 50.0f;\n";
            out << "    state." << na.node->id << ".Dv = 50.0f;\n";
            out << "    state." << na.node->id << ".Dw = 50.0f;\n";
        }
        // (Plant.Fault.Point: outputs zero-init; the host raises them.)
    }
    out << "}\n\n";

    // ----- PlantStep -----
    out << "void PlantStep(PlantState& state, double dt) {\n";
    std::size_t stepIndex = 0;
    for (const auto& id : order) {
        const NodeAnalysis& na = byId(id);
        if (na.stepBody.empty()) {
            out << "    // ---- step " << stepIndex++ << ": " << na.node->id << " ("
                << na.node->type << ") — collector bound in PlantCollectMeas ----\n";
            continue;
        }
        out << "    // ---- step " << stepIndex++ << ": " << na.node->id << " ("
            << na.node->type << ") ----\n";
        out << EmitBodyBlock(na, "    ");
    }
    if (order.empty()) out << "    (void)state; (void)dt;\n";
    out << "}\n\n";

    // ----- PlantCollectMeas -----
    out << "void PlantCollectMeas(const PlantState& state, PlantMeas& out) {\n";
    const NodeAnalysis* measNode = nullptr;
    for (const auto& na : a.nodes) {
        if (na.node->type == "Plant.Io.MeasOut") {
            measNode = &na;
            break;
        }
    }
    std::vector<std::string> measInputs;
    for (const auto& type : a.graph.GetNodeTypes()) {
        if (type.id != "Plant.Io.MeasOut") continue;
        for (const auto& p : type.inputPorts) measInputs.push_back(p.name);
    }
    for (const auto& name : measInputs) {
        if (measNode && measNode->wiredInputs.count(name)) {
            const auto& producer = measNode->wiredInputs.at(name);
            out << "    out." << name << " = state." << producer.nodeId << "."
                << producer.portName << ";\n";
        } else {
            out << "    out." << name
                << " = std::numeric_limits<float>::quiet_NaN();\n";
        }
    }
    if (measInputs.empty()) out << "    (void)state; (void)out;\n";
    out << "}\n\n";

    // ----- Host externals -----
    const auto nodesOfType = [&](const char* typeId) {
        std::vector<const NodeAnalysis*> found;
        for (const auto& na : a.nodes) {
            if (na.node->type == typeId) found.push_back(&na);
        }
        return found;
    };

    out << "void PlantSetDuty(PlantState& state, float du, float dv, float dw) {\n";
    const auto dutyNodes = nodesOfType("Plant.Io.DutyIn");
    if (dutyNodes.empty()) {
        out << "    (void)state; (void)du; (void)dv; (void)dw;\n";
    } else {
        for (const auto* na : dutyNodes) {
            out << "    state." << na->node->id << ".Du = du;\n";
            out << "    state." << na->node->id << ".Dv = dv;\n";
            out << "    state." << na->node->id << ".Dw = dw;\n";
        }
    }
    out << "}\n\n";

    out << "void PlantSetDynoRpm(PlantState& state, float rpm) {\n";
    const auto dynoNodes = nodesOfType("Plant.Shaft.Dyno");
    if (dynoNodes.empty()) {
        out << "    (void)state; (void)rpm;\n";
    } else {
        for (const auto* na : dynoNodes) {
            // Assign Rpm, then publish the derived views (python host emit).
            out << "    state." << na->node->id << ".Rpm = rpm;\n";
            out << "    {\n";
            std::set<std::string> used = na->usedStep;
            used.insert("Rpm");
            EmitParameterBinds(out, *na, used, "        ");
            EmitStateBinds(out, *na, used, "        ");
            out << na->stepBody;
            if (!na->stepBody.empty() && na->stepBody.back() != '\n') out << "\n";
            out << "    }\n";
        }
    }
    out << "}\n\n";

    out << "void PlantSetFault(PlantState& state, const char* name, bool active) {\n";
    const auto faultNodes = nodesOfType("Plant.Fault.Point");
    if (faultNodes.empty()) {
        out << "    (void)state; (void)name; (void)active;\n";
    } else {
        out << "    const float value = active ? 1.0f : 0.0f;\n";
        for (const auto* na : faultNodes) {
            out << "    // " << na->node->id << " (unknown names are ignored)\n";
            for (const auto& port : na->outputPorts) {
                out << "    if (std::strcmp(name, \"" << port << "\") == 0) state."
                    << na->node->id << "." << port << " = value;\n";
            }
        }
    }
    out << "}\n\n";

    out << "bool PlantSetInput(PlantState& state, const char* portName, float value) {\n";
    bool anyTargets = false;
    for (const auto& na : a.nodes) {
        if (na.node->type == "Plant.Io.MeasOut" || na.cls == NodeClass::kHost) continue;
        for (const auto& name : na.unwiredInputs) {
            out << "    if (std::strcmp(portName, \"" << name << "\") == 0) {\n"
                << "        state." << na.node->id << "." << name << " = value;\n"
                << "        return true;\n"
                << "    }\n";
            anyTargets = true;
        }
    }
    if (!anyTargets) {
        out << "    (void)state; (void)portName; (void)value;\n";
    } else {
        out << "    (void)portName; (void)value;\n";
    }
    out << "    return false;\n";
    out << "}\n\n";

    out << "void PlantSetSupply(PlantState& state, double vSet, double iLimit,\n"
           "                      double sinkLimit, bool canSink) {\n";
    const auto busNodes = nodesOfType("Plant.DcBus");
    if (busNodes.empty()) {
        out << "    (void)state; (void)vSet; (void)iLimit; (void)sinkLimit; (void)canSink;\n";
    } else {
        for (const auto* na : busNodes) {
            out << "    state." << na->node->id << ".VSet_V = vSet;\n";
            out << "    state." << na->node->id << ".ILimit_A = iLimit;\n";
            out << "    state." << na->node->id << ".SinkLimit_A = sinkLimit;\n";
            out << "    state." << na->node->id << ".CanSink = canSink ? 1.0 : 0.0;\n";
        }
    }
    out << "}\n\n";

    out << "}  // namespace plant\n";
    return out.str();
}

}  // namespace

PlantGenerator::PlantGenerator(const NodeAPI::Graph& graph) : graph_(graph) {}

bool PlantGenerator::Generate(const std::string& outputDir, std::string& error) const {
    Analysis analysis = Analyze(graph_);
    if (!Validate(analysis, error)) return false;

    std::vector<std::string> order;
    if (!ComputeSchedule(analysis, order, error)) return false;

    std::error_code ec;
    std::filesystem::create_directories(outputDir, ec);
    if (ec) {
        error = "could not create output directory '" + outputDir + "': " +
                ec.message();
        return false;
    }
    const std::filesystem::path out(outputDir);
    if (!WriteFile(out / "plant_generated.h", BuildHeader(analysis), error)) {
        return false;
    }
    if (!WriteFile(out / "plant_generated.cpp", BuildSource(analysis, order), error)) {
        return false;
    }
    return true;
}

}  // namespace PlantCodegen
