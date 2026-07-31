#include <InverterCodegen/CodeGenerator.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace InverterCodegen {

namespace {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

std::string SanitizeIdentifier(std::string_view id) {
    std::string out;
    for (size_t i = 0; i < id.size(); ++i) {
        char c = id[i];
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            out += c;
        } else {
            out += '_';
        }
    }
    if (!out.empty() && std::isdigit(static_cast<unsigned char>(out[0]))) {
        out = "_" + out;
    }
    return out;
}

bool IsValidIdentifier(std::string_view id) {
    if (id.empty()) return false;
    if (std::isdigit(static_cast<unsigned char>(id[0]))) return false;
    for (char c : id) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
    }
    return true;
}

std::string Capitalize(std::string_view s) {
    if (s.empty()) return "";
    std::string out;
    bool upperNext = true;
    for (char c : s) {
        if (c == '_' || c == ' ' || c == '-') {
            upperNext = true;
        } else if (upperNext) {
            out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            upperNext = false;
        } else {
            out += c;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Identifier extraction
// ---------------------------------------------------------------------------

std::vector<std::string> ExtractAllIdentifiers(const std::string& code) {
    std::vector<std::string> result;
    std::regex re(R"(\b([A-Za-z_][A-Za-z0-9_]*)\b)");
    auto begin = std::sregex_iterator(code.begin(), code.end(), re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        result.push_back(it->str(1));
    }
    return result;
}

std::unordered_set<std::string> UsedIdentifiers(const std::string& code) {
    auto ids = ExtractAllIdentifiers(code);
    return std::unordered_set<std::string>(ids.begin(), ids.end());
}

// ---------------------------------------------------------------------------
// Graph analysis
// ---------------------------------------------------------------------------

std::unordered_map<std::string, std::vector<NodeAPI::Node>> GroupNodesByDomain(
    const NodeAPI::Graph& graph) {
    std::unordered_map<std::string, std::vector<NodeAPI::Node>> result;
    for (const auto& node : graph.GetNodes()) {
        result[node.domain].push_back(node);
    }
    return result;
}

std::vector<std::string> TopologicalSort(
    const std::vector<NodeAPI::Node>& nodes,
    const NodeAPI::Graph& graph,
    std::string& error) {
    std::unordered_map<std::string, size_t> indexById;
    for (size_t i = 0; i < nodes.size(); ++i) {
        indexById[nodes[i].id] = i;
    }

    std::vector<std::vector<size_t>> dependents(nodes.size());
    std::vector<size_t> incoming(nodes.size(), 0);

    for (const auto& connection : graph.GetConnections()) {
        auto fromIt = indexById.find(connection.from.nodeId);
        auto toIt = indexById.find(connection.to.nodeId);
        if (fromIt == indexById.end() || toIt == indexById.end()) continue;

        dependents[fromIt->second].push_back(toIt->second);
        ++incoming[toIt->second];
    }

    std::vector<size_t> queue;
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (incoming[i] == 0) queue.push_back(i);
    }

    std::vector<std::string> order;
    order.reserve(nodes.size());

    size_t processed = 0;
    while (processed < queue.size()) {
        size_t idx = queue[processed++];
        order.push_back(nodes[idx].id);

        for (size_t dep : dependents[idx]) {
            if (--incoming[dep] == 0) {
                queue.push_back(dep);
            }
        }
    }

    if (order.size() != nodes.size()) {
        error = "Cycle detected in domain '" + nodes.front().domain + "'";
        return {};
    }

    return order;
}

std::optional<std::string> FindSourceExpression(
    const NodeAPI::Graph& graph,
    const std::string& targetNodeId,
    const std::string& targetPortName) {
    for (const auto& connection : graph.GetConnections()) {
        if (connection.to.nodeId == targetNodeId && connection.to.portName == targetPortName) {
            return "state." + connection.from.nodeId + "." + connection.from.portName;
        }
    }
    // Check for a cross-domain bridge feeding this input.
    for (const auto& bridge : graph.GetBridges()) {
        if (bridge.consumer.nodeId == targetNodeId && bridge.consumer.portName == targetPortName) {
            return "Bridge" + Capitalize(bridge.id) + ".load()";
        }
    }
    return std::nullopt;
}

std::vector<NodeAPI::Bridge> FindProducerBridges(
    const NodeAPI::Graph& graph,
    const std::string& targetNodeId,
    const std::string& targetPortName) {
    std::vector<NodeAPI::Bridge> result;
    for (const auto& bridge : graph.GetBridges()) {
        if (bridge.producer.nodeId == targetNodeId && bridge.producer.portName == targetPortName) {
            result.push_back(bridge);
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Type helpers (placeholder until real unit library is chosen)
// ---------------------------------------------------------------------------

std::string WireTypeToCpp(const NodeAPI::WireType& type) {
    // Scalar quantities map directly to rte aliases. Framed quantities use the
    // corresponding ABC / alpha-beta / DQ structs in RteQuantity.h.
    switch (type.quantity) {
        case NodeAPI::Quantity::Voltage:
            switch (type.frame) {
                case NodeAPI::Frame::Abc: return "rte::AbcVoltage";
                case NodeAPI::Frame::AlphaBeta: return "rte::AlphaBetaVoltage";
                case NodeAPI::Frame::Dq: return "rte::DqVoltage";
                case NodeAPI::Frame::Scalar: return "rte::Voltage";
            }
            break;
        case NodeAPI::Quantity::Current:
            switch (type.frame) {
                case NodeAPI::Frame::Abc: return "rte::AbcCurrent";
                case NodeAPI::Frame::AlphaBeta: return "rte::AlphaBetaCurrent";
                case NodeAPI::Frame::Dq: return "rte::DqCurrent";
                case NodeAPI::Frame::Scalar: return "rte::Current";
            }
            break;
        case NodeAPI::Quantity::AngularVelocity:
            return "rte::AngularVelocity";
        case NodeAPI::Quantity::Torque:
            return "rte::Torque";
        case NodeAPI::Quantity::Temperature:
            return "rte::Temperature";
        case NodeAPI::Quantity::Dimensionless:
            return "rte::Dimensionless";
        case NodeAPI::Quantity::Boolean:
            return "rte::Boolean";
        case NodeAPI::Quantity::String:
            return "const char*";
    }
    return "float";
}

std::string ParameterValueToCpp(const NodeAPI::WireType& type, const std::string& value) {
    // Emit a C++ expression that constructs the right unit-wrapped value from a
    // plain numeric literal. Framed parameter types are not supported.
    switch (type.quantity) {
        case NodeAPI::Quantity::Voltage:
            if (type.frame != NodeAPI::Frame::Scalar) break;
            return "rte::Volts(" + value + "f)";
        case NodeAPI::Quantity::Current:
            if (type.frame != NodeAPI::Frame::Scalar) break;
            return "rte::Amperes(" + value + "f)";
        case NodeAPI::Quantity::AngularVelocity:
            return "rte::RadiansPerSecond(" + value + "f)";
        case NodeAPI::Quantity::Torque:
            return "rte::NewtonMeters(" + value + "f)";
        case NodeAPI::Quantity::Temperature:
            return "rte::Celsius(" + value + "f)";
        case NodeAPI::Quantity::Dimensionless:
        case NodeAPI::Quantity::Boolean:
            return value + "f";
        case NodeAPI::Quantity::String:
            // String params are stored unquoted in the graph JSON; quote them
            // for C++ emission.
            return "\"" + value + "\"";
    }
    // Unknown / framed: fall back to a plain float so existing templates keep
    // working, but this loses unit safety for those parameters.
    return value + "f";
}

// Config nodes (type id "config.*") expose a FRAM-backed value by a user-chosen
// string key. They must declare a string parameter "Key" and a scalar parameter
// "Cached" (the live RAM state backing the node's output).
bool IsConfigNodeType(const NodeAPI::NodeType& nodeType) {
    return nodeType.id.rfind("config.", 0) == 0 || nodeType.id == "Values.Config";
}

// Returns the config key, or nullopt when the node has no valid non-empty
// string "Key" parameter (keys are stored unquoted in the graph JSON).
std::optional<std::string> ConfigNodeKey(const NodeAPI::Node& node) {
    const auto it = node.parameters.find("Key");
    if (it == node.parameters.end()) return std::nullopt;
    const std::string& raw = it->second;
    if (raw.empty() || raw.find('"') != std::string::npos) return std::nullopt;
    return raw;
}

/* Unit extraction for implicit conversions: when a dimensionless scalar
 * input binds from a voltage/current/temperature scalar source (direct
 * connection), the emitted expression needs the .in(unit) suffix to unwrap
 * the au quantity. */
std::string ExtractionSuffix(const NodeAPI::Graph& graph,
                             const std::string& targetNodeId,
                             const std::string& targetPortName) {
    const auto targetNode = graph.FindNode(targetNodeId);
    if (!targetNode) return "";
    const auto targetType = graph.FindNodeType(targetNode->type);
    if (!targetType) return "";
    const auto targetPort = targetType->FindInputPort(targetPortName);
    if (!targetPort) return "";
    if (targetPort->type.quantity != NodeAPI::Quantity::Dimensionless ||
        targetPort->type.frame != NodeAPI::Frame::Scalar) {
        return "";
    }

    for (const auto& c : graph.GetConnections()) {
        if (c.to.nodeId == targetNodeId && c.to.portName == targetPortName) {
            const auto srcNode = graph.FindNode(c.from.nodeId);
            if (!srcNode) return "";
            const auto srcType = graph.FindNodeType(srcNode->type);
            if (!srcType) return "";
            const auto srcPort = srcType->FindOutputPort(c.from.portName);
            if (!srcPort) return "";
            if (srcPort->type.quantity == NodeAPI::Quantity::Voltage) {
                return ".in(au::volts)";
            }
            if (srcPort->type.quantity == NodeAPI::Quantity::Current) {
                return ".in(au::amperes)";
            }
            if (srcPort->type.quantity == NodeAPI::Quantity::Temperature) {
                return ".in(au::celsius)";
            }
            return "";
        }
    }
    return "";
}

/* Unit injection (mirror of ExtractionSuffix): when a physical-quantity
 * scalar input binds from a dimensionless scalar source, the emitted
 * expression must be wrapped in the input's unit.  Returns the wrapper
 * function name (e.g. "rte::Amperes") or "" when no injection applies. */
std::string InjectionWrapper(const NodeAPI::Graph& graph,
                             const std::string& targetNodeId,
                             const std::string& targetPortName) {
    const auto targetNode = graph.FindNode(targetNodeId);
    if (!targetNode) return "";
    const auto targetType = graph.FindNodeType(targetNode->type);
    if (!targetType) return "";
    const auto targetPort = targetType->FindInputPort(targetPortName);
    if (!targetPort) return "";
    const auto q = targetPort->type.quantity;
    if (targetPort->type.frame != NodeAPI::Frame::Scalar ||
        q == NodeAPI::Quantity::Dimensionless ||
        q == NodeAPI::Quantity::Boolean) {
        return "";
    }

    for (const auto& c : graph.GetConnections()) {
        if (c.to.nodeId == targetNodeId && c.to.portName == targetPortName) {
            const auto srcNode = graph.FindNode(c.from.nodeId);
            if (!srcNode) return "";
            const auto srcType = graph.FindNodeType(srcNode->type);
            if (!srcType) return "";
            const auto srcPort = srcType->FindOutputPort(c.from.portName);
            if (!srcPort || srcPort->type.quantity != NodeAPI::Quantity::Dimensionless ||
                srcPort->type.frame != NodeAPI::Frame::Scalar) {
                return "";
            }
            switch (q) {
                case NodeAPI::Quantity::Voltage:         return "rte::Volts";
                case NodeAPI::Quantity::Current:         return "rte::Amperes";
                case NodeAPI::Quantity::Temperature:     return "rte::Celsius";
                case NodeAPI::Quantity::Torque:          return "rte::NewtonMeters";
                case NodeAPI::Quantity::AngularVelocity: return "rte::RadiansPerSecond";
                default:                                 return "";
            }
        }
    }
    return "";
}

// Var nodes (type id "Values.Var*") hold machine-owned RAM state in their
// "Stored" parameter; a runtime registry lets the shell adjust them by node id.
bool IsVarNodeType(const NodeAPI::NodeType& nodeType) {
    return nodeType.id.rfind("Values.Var", 0) == 0;
}

/* True when the instance flags this parameter as a parameterInput (bound
 * from a connection like an input port instead of a constant). */
bool IsParameterInput(const NodeAPI::Node& node, const std::string& key) {
    for (const auto& name : node.parameterInputs) {
        if (name == key) return true;
    }
    return false;
}

std::optional<std::string> ExtractClassName(const std::string& classHeader) {
    // Very simple parser: find "class <Name>" or "class <Namespace::Name>".
    std::regex re(R"(\bclass\s+([A-Za-z_][A-Za-z0-9_:]*)\s*[:\{])");
    std::smatch match;
    if (std::regex_search(classHeader, match, re)) {
        return match.str(1);
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Code emission helpers
// ---------------------------------------------------------------------------

bool WriteFile(const std::filesystem::path& path, const std::string& content, std::string& error) {
    std::ofstream file(path);
    if (!file.is_open()) {
        error = "Failed to open file for writing: " + path.string();
        return false;
    }
    file << content;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// CodeGenerator implementation
// ---------------------------------------------------------------------------

CodeGenerator::CodeGenerator(const NodeAPI::Graph& graph) : graph_(graph) {}

namespace {

std::string BuildStateStruct(
    const std::string& domainTitle,
    const std::vector<std::string>& order,
    const NodeAPI::Graph& graph,
    std::string& error) {
    std::ostringstream source;
    source << "struct " << domainTitle << "State {\n";
    for (const auto& nodeId : order) {
        const auto node = graph.FindNode(nodeId);
        if (!node) continue;
        const auto nodeType = graph.FindNodeType(node->type);
        if (!nodeType) continue;

        const bool classBased = !nodeType->classHeader.empty() || !nodeType->classDefinition.empty();

        const std::string nodeTitle = Capitalize(node->id);
        source << "    struct " << nodeTitle << "State {\n";

        // Output members.
        for (const auto& port : nodeType->outputPorts) {
            source << "        " << WireTypeToCpp(port.type) << " " << port.name << ";\n";
        }

        if (classBased) {
            auto className = ExtractClassName(nodeType->classHeader);
            if (!className) {
                error = "NodeType '" + nodeType->id +
                        "' has classHeader but no recognizable class name";
                return {};
            }
            source << "        " << *className << " instance;\n";
        } else {
            // Function-style: only declared parameters become persistent state
            // members. Everything else in inlineCode/constructorCode is treated as
            // a local variable declared by the template author.  Parameters
            // flagged as parameterInputs are wire-bound and get no state member.
            for (const auto& [key, value] : node->parameters) {
                if (IsParameterInput(*node, key)) continue;
                auto paramType = nodeType->FindParameterType(key);
                source << "        "
                       << (paramType ? WireTypeToCpp(*paramType) : "rte::Dimensionless") << " "
                       << key << ";\n";
            }
        }

        source << "    };\n";
        source << "\n";
        source << "    " << nodeTitle << "State " << node->id << ";\n";
        source << "\n";
    }
    source << "};\n";
    return source.str();
}

}  // namespace

bool CodeGenerator::Generate(const std::string& outputDir, std::string& error) const {
    std::filesystem::path outPath(outputDir);
    std::filesystem::create_directories(outPath);

    // Validate node ids.
    for (const auto& node : graph_.GetNodes()) {
        if (!IsValidIdentifier(node.id)) {
            error = "Node id is not a valid C++ identifier: " + node.id;
            return false;
        }
    }

    auto domains = GroupNodesByDomain(graph_);

    for (const auto& [domain, nodes] : domains) {
        if (domain.empty()) {
            error = "Domain name is empty for one or more nodes";
            return false;
        }

        const std::string domainCpp = SanitizeIdentifier(domain);
        const std::string domainTitle = Capitalize(domainCpp);

        std::string domainError;
        auto order = TopologicalSort(nodes, graph_, domainError);
        if (!domainError.empty()) {
            error = domainError;
            return false;
        }

        // Collect node types used in this domain.
        std::unordered_set<std::string> typeIdsUsed;
        for (const auto& node : nodes) {
            typeIdsUsed.insert(node.type);
        }

        std::string stateStruct = BuildStateStruct(domainTitle, order, graph_, error);
        if (!error.empty()) return false;

        // Build header.
        std::ostringstream header;
        header << "#pragma once\n\n";
        header << "// Generated by InverterCodegen. Do not edit by hand.\n\n";
        header << "#include <stdint.h>\n";
        header << "#include \"InverterCodegen/RteQuantity.h\"\n";
        if (!graph_.GetBridges().empty()) {
            header << "#include \"bridges_generated.h\"\n";
        }
        header << "\n";

        for (const auto& typeId : typeIdsUsed) {
            const auto nodeType = graph_.FindNodeType(typeId);
            if (!nodeType) continue;
            if (!nodeType->classHeader.empty()) {
                header << "// From node type: " << typeId << "\n";
                header << nodeType->classHeader << "\n\n";
            }
        }

        header << "namespace app {\n\n";
        header << stateStruct << "\n\n";
        header << "void " << domainTitle << "Init(" << domainTitle << "State& state);\n";
        header << "void " << domainTitle << "Step(" << domainTitle << "State& state);\n";
        header << "void " << domainTitle << "Start(" << domainTitle << "State& state);\n";
        header << "void " << domainTitle << "Stop(" << domainTitle << "State& state);\n";
        header << "\n} // namespace app\n\n";
        header << "#include \"RteParams.h\"\n";
        header << "namespace app {\n";
        header << "extern const RteParamDesc g_" << domainCpp << "_configs[];\n";
        header << "extern const size_t g_" << domainCpp << "_config_count;\n";
        header << "extern const RteParamDesc g_" << domainCpp << "_vars[];\n";
        header << "extern const size_t g_" << domainCpp << "_var_count;\n";
        header << "} // namespace app\n";

        // Build source.
        std::ostringstream source;
        source << "// Generated by InverterCodegen. Do not edit by hand.\n\n";
        source << "#include \"domain_" << domainCpp << "_generated.h\"\n";
        source << "#include \"platform_api.h\"\n\n";

        for (const auto& typeId : typeIdsUsed) {
            const auto nodeType = graph_.FindNodeType(typeId);
            if (!nodeType) continue;
            if (!nodeType->classDefinition.empty()) {
                source << "// From node type: " << typeId << "\n";
                source << nodeType->classDefinition << "\n\n";
            }
        }

        source << "namespace app {\n\n";

        // Init function.
        source << "void " << domainTitle << "Init(" << domainTitle << "State& state) {\n";
        for (const auto& nodeId : order) {
            const auto node = graph_.FindNode(nodeId);
            if (!node) continue;
            const auto nodeType = graph_.FindNodeType(node->type);
            if (!nodeType) continue;

            const bool classBased = !nodeType->classHeader.empty() || !nodeType->classDefinition.empty();

            source << "    // Init node: " << node->id << "\n";
            source << "    {\n";

            if (classBased) {
                source << "        auto& instance = state." << node->id << ".instance;\n";
                auto used = UsedIdentifiers(nodeType->constructorCode);
                for (const auto& [key, value] : node->parameters) {
                    if (used.count(key)) {
                        auto paramType = nodeType->FindParameterType(key);
                        source << "        const "
                               << (paramType ? WireTypeToCpp(*paramType) : "rte::Dimensionless")
                               << " " << key << " = "
                               << (paramType ? ParameterValueToCpp(*paramType, value)
                                             : value + "f")
                               << ";\n";
                    }
                }
            } else {
                for (const auto& [key, value] : node->parameters) {
                    if (IsParameterInput(*node, key)) continue;
                    auto paramType = nodeType->FindParameterType(key);
                    source << "        state." << node->id << "." << key << " = "
                           << (paramType ? ParameterValueToCpp(*paramType, value) : value + "f")
                           << ";\n";
                }
                /* Bind local refs for parameters used by constructorCode. */
                if (!nodeType->constructorCode.empty()) {
                    auto used = UsedIdentifiers(nodeType->constructorCode);
                    for (const auto& [key, value] : node->parameters) {
                        if (IsParameterInput(*node, key)) continue;
                        if (used.count(key)) {
                            auto paramType = nodeType->FindParameterType(key);
                            source << "        "
                                   << (paramType ? WireTypeToCpp(*paramType) : "rte::Dimensionless")
                                   << "& " << key << " = state." << node->id << "." << key << ";\n";
                        }
                    }
                }
            }

            if (!nodeType->constructorCode.empty()) {
                source << "        " << nodeType->constructorCode << "\n";
            }

            source << "    }\n";
        }
        source << "}\n\n";

        // Step function.
        source << "void " << domainTitle << "Step(" << domainTitle << "State& state) {\n";
        for (const auto& nodeId : order) {
            const auto node = graph_.FindNode(nodeId);
            if (!node) continue;
            const auto nodeType = graph_.FindNodeType(node->type);
            if (!nodeType) continue;

            const bool classBased = !nodeType->classHeader.empty() || !nodeType->classDefinition.empty();
            if (classBased && !node->parameterInputs.empty()) {
                error = "parameterInputs are not supported on class-based node '" +
                        node->id + "'";
                return false;
            }

            source << "    // Step node: " << node->id << " (" << nodeType->id << ")\n";
            source << "    {\n";

            // Inputs.
            for (const auto& port : nodeType->inputPorts) {
                auto src = FindSourceExpression(graph_, node->id, port.name);
                if (!src) {
                    if (port.optional) {
                        /* Optional unconnected input: bind a const ref to the
                         * parameter with the same name, if it exists. */
                        const auto paramIt = node->parameters.find(port.name);
                        if (paramIt != node->parameters.end()) {
                            auto paramType = nodeType->FindParameterType(port.name);
                            source << "        const "
                                   << (paramType ? WireTypeToCpp(*paramType) : "rte::Dimensionless")
                                   << "& " << port.name << " = state." << node->id << "." << port.name << ";\n";
                        }
                        continue;
                    }
                    error = "Input port '" + port.name + "' of node '" + node->id +
                            "' is not connected";
                    return false;
                }
                const std::string wrap = InjectionWrapper(graph_, node->id, port.name);
                if (!wrap.empty()) {
                    source << "        const " << WireTypeToCpp(port.type) << " " << port.name
                           << " = " << wrap << "(" << *src << ");\n";
                } else {
                    source << "        const " << WireTypeToCpp(port.type) << " " << port.name
                           << " = " << *src
                           << ExtractionSuffix(graph_, node->id, port.name) << ";\n";
                }
            }

            // Parameters flagged as parameterInputs bind from connections too.
            for (const auto& key : node->parameterInputs) {
                auto src = FindSourceExpression(graph_, node->id, key);
                if (!src) {
                    error = "parameterInput '" + key + "' of node '" + node->id +
                            "' is not connected";
                    return false;
                }
                auto paramType = nodeType->FindParameterType(key);
                source << "        const "
                       << (paramType ? WireTypeToCpp(*paramType) : "rte::Dimensionless")
                       << " " << key << " = " << *src
                       << ExtractionSuffix(graph_, node->id, key) << ";\n";
            }

            // Outputs.
            for (const auto& port : nodeType->outputPorts) {
                source << "        " << WireTypeToCpp(port.type) << "& " << port.name
                       << " = state." << node->id << "." << port.name << ";\n";
            }

            if (classBased) {
                source << "        auto& instance = state." << node->id << ".instance;\n";
                auto used = UsedIdentifiers(nodeType->inlineCode);
                for (const auto& [key, value] : node->parameters) {
                    if (used.count(key)) {
                        auto paramType = nodeType->FindParameterType(key);
                        source << "        const "
                               << (paramType ? WireTypeToCpp(*paramType) : "rte::Dimensionless")
                               << " " << key << " = "
                               << (paramType ? ParameterValueToCpp(*paramType, value)
                                             : value + "f")
                               << ";\n";
                    }
                }
            } else {
                // Bind parameters as mutable refs (they may be updated by the
                // inline code, e.g. an integrator). Inputs are already const.
                for (const auto& [key, value] : node->parameters) {
                    /* Skip parameters shadowed by a same-named input port: the
                     * port path already binds that name (from the wire when
                     * connected, or as a const ref to this parameter when the
                     * port is optional and unconnected). */
                    bool shadowed = false;
                    for (const auto& port : nodeType->inputPorts) {
                        if (port.name == key) {
                            shadowed = true;
                            break;
                        }
                    }
                    if (shadowed) continue;
                    /* parameterInputs are bound from their connection earlier. */
                    if (IsParameterInput(*node, key)) continue;

                    auto paramType = nodeType->FindParameterType(key);
                    source << "        "
                           << (paramType ? WireTypeToCpp(*paramType) : "rte::Dimensionless")
                           << "& " << key << " = state." << node->id << "." << key << ";\n";
                }
            }

            if (!nodeType->inlineCode.empty()) {
                source << "        " << nodeType->inlineCode << "\n";
            }

            // Cross-domain bridges: write any produced values to bridge variables.
            for (const auto& port : nodeType->outputPorts) {
                auto producerBridges = FindProducerBridges(graph_, node->id, port.name);
                for (const auto& bridge : producerBridges) {
                    std::string suffix;
                    if (bridge.type.quantity == NodeAPI::Quantity::Dimensionless) {
                        if (port.type.quantity == NodeAPI::Quantity::Voltage) {
                            suffix = ".in(au::volts)";
                        } else if (port.type.quantity == NodeAPI::Quantity::Current) {
                            suffix = ".in(au::amperes)";
                        }
                    }
                    source << "        Bridge" << Capitalize(bridge.id)
                           << ".store(" << port.name << suffix << ");\n";
                }
            }

            source << "    }\n";
        }
        source << "}\n\n";

        // Start function: reset mutable node state to graph defaults.
        source << "void " << domainTitle << "Start(" << domainTitle << "State& state) {\n";
        for (const auto& nodeId : order) {
            const auto node = graph_.FindNode(nodeId);
            if (!node) continue;
            const auto nodeType = graph_.FindNodeType(node->type);
            if (!nodeType) continue;

            const bool classBased = !nodeType->classHeader.empty() || !nodeType->classDefinition.empty();
            if (classBased) continue;  // class-based nodes manage their own reset

            // Config nodes: never reset Cached to the graph's placeholder 0.
            // Re-load from FRAM (or DefaultValue) so control start cannot zero
            // Motor.Poles / Ld / Lq / encoder offset and freeze θe at 0.
            if (IsConfigNodeType(*nodeType)) {
                source << "    // Reload config: " << node->id << "\n";
                source << "    {\n";
                for (const auto& [key, value] : node->parameters) {
                    if (key == "Cached") continue;  // filled by constructor / load
                    auto paramType = nodeType->FindParameterType(key);
                    source << "        state." << node->id << "." << key << " = "
                           << (paramType ? ParameterValueToCpp(*paramType, value)
                                         : value + "f")
                           << ";\n";
                }
                if (!nodeType->constructorCode.empty()) {
                    auto used = UsedIdentifiers(nodeType->constructorCode);
                    for (const auto& [key, value] : node->parameters) {
                        if (used.count(key)) {
                            auto paramType = nodeType->FindParameterType(key);
                            source << "        "
                                   << (paramType ? WireTypeToCpp(*paramType)
                                                 : "rte::Dimensionless")
                                   << "& " << key << " = state." << node->id << "."
                                   << key << ";\n";
                        }
                    }
                    source << "        " << nodeType->constructorCode << "\n";
                }
                source << "    }\n";
                continue;
            }

            source << "    // Reset node: " << node->id << "\n";
            source << "    {\n";
            for (const auto& [key, value] : node->parameters) {
                if (IsParameterInput(*node, key)) continue;
                auto paramType = nodeType->FindParameterType(key);
                source << "        state." << node->id << "." << key << " = "
                       << (paramType ? ParameterValueToCpp(*paramType, value) : value + "f")
                       << ";\n";
            }
            source << "    }\n";
        }
        source << "}\n\n";

        // Stop function: zero all output ports.
        source << "void " << domainTitle << "Stop(" << domainTitle << "State& state) {\n";
        for (const auto& nodeId : order) {
            const auto node = graph_.FindNode(nodeId);
            if (!node) continue;
            const auto nodeType = graph_.FindNodeType(node->type);
            if (!nodeType) continue;

            source << "    // Zero node: " << node->id << "\n";
            source << "    {\n";
            for (const auto& port : nodeType->outputPorts) {
                source << "        state." << node->id << "." << port.name
                       << " = " << WireTypeToCpp(port.type) << "{};\n";
            }
            source << "    }\n";
        }
        source << "}\n\n";

        // Config registry: expose config.* nodes' cached state by their
        // user-chosen FRAM key for runtime get/set/save.
        size_t configCount = 0;
        std::ostringstream configEntries;
        for (const auto& nodeId : order) {
            const auto node = graph_.FindNode(nodeId);
            if (!node) continue;
            const auto nodeType = graph_.FindNodeType(node->type);
            if (!nodeType || !IsConfigNodeType(*nodeType)) continue;

            const auto configKey = ConfigNodeKey(*node);
            if (!configKey) {
                error = "Config node '" + node->id +
                        "' needs a non-empty string 'Key' parameter";
                return false;
            }
            if (configKey->empty() || configKey->size() > 32) {
                error = "Config node '" + node->id + "' key must be 1..32 chars: '" +
                        *configKey + "'";
                return false;
            }
            if (node->parameters.find("Cached") == node->parameters.end()) {
                error = "Config node '" + node->id + "' needs a 'Cached' parameter";
                return false;
            }

            const std::string setterName = "set_" + node->id + "_cached";
            const std::string getterName = "get_" + node->id + "_cached";
            source << "static void " << setterName << "(void* state, float value) {\n";
            source << "    auto* s = static_cast<" << domainTitle << "State*>(state);\n";
            source << "    s->" << node->id << ".Cached = value;\n";
            source << "}\n\n";
            source << "static float " << getterName << "(const void* state) {\n";
            source << "    const auto* s = static_cast<const " << domainTitle
                   << "State*>(state);\n";
            source << "    return s->" << node->id << ".Cached;\n";
            source << "}\n\n";

            configEntries << "    {\"" << *configKey << "\", " << setterName << ", "
                          << getterName << "},\n";
            ++configCount;
        }
        source << "const RteParamDesc g_" << domainCpp << "_configs[] = {\n";
        if (configCount == 0) {
            // Zero-length arrays are not legal C++; emit a sentinel and count 0.
            source << "    {nullptr, nullptr, nullptr},\n";
        } else {
            source << configEntries.str();
        }
        source << "};\n\n";
        source << "const size_t g_" << domainCpp << "_config_count = " << configCount
               << ";\n\n";

        // Var registry: expose var.* nodes' Stored state by node id for
        // runtime adjustment (RAM-only, machine-owned).
        size_t varCount = 0;
        std::ostringstream varEntries;
        for (const auto& nodeId : order) {
            const auto node = graph_.FindNode(nodeId);
            if (!node) continue;
            const auto nodeType = graph_.FindNodeType(node->type);
            if (!nodeType || !IsVarNodeType(*nodeType)) continue;
            if (node->parameters.find("Stored") == node->parameters.end()) continue;

            auto storedType = nodeType->FindParameterType("Stored");
            const auto storedQty = storedType ? storedType->quantity
                                              : NodeAPI::Quantity::Dimensionless;

            const std::string setterName = "set_" + node->id + "_stored";
            const std::string getterName = "get_" + node->id + "_stored";
            source << "static void " << setterName << "(void* state, float value) {\n";
            source << "    auto* s = static_cast<" << domainTitle << "State*>(state);\n";
            if (storedQty == NodeAPI::Quantity::Boolean) {
                source << "    s->" << node->id << ".Stored = (value != 0.0f);\n";
            } else if (storedQty == NodeAPI::Quantity::Current) {
                source << "    s->" << node->id << ".Stored = rte::Amperes(value);\n";
            } else if (storedQty == NodeAPI::Quantity::Voltage) {
                source << "    s->" << node->id << ".Stored = rte::Volts(value);\n";
            } else {
                source << "    s->" << node->id << ".Stored = value;\n";
            }
            source << "}\n\n";
            source << "static float " << getterName << "(const void* state) {\n";
            source << "    const auto* s = static_cast<const " << domainTitle
                   << "State*>(state);\n";
            if (storedQty == NodeAPI::Quantity::Boolean) {
                source << "    return s->" << node->id << ".Stored ? 1.0f : 0.0f;\n";
            } else if (storedQty == NodeAPI::Quantity::Current) {
                source << "    return s->" << node->id << ".Stored.in(au::amperes);\n";
            } else if (storedQty == NodeAPI::Quantity::Voltage) {
                source << "    return s->" << node->id << ".Stored.in(au::volts);\n";
            } else {
                source << "    return s->" << node->id << ".Stored;\n";
            }
            source << "}\n\n";

            varEntries << "    {\"" << node->id << "\", " << setterName << ", "
                       << getterName << "},\n";
            ++varCount;
        }
        source << "const RteParamDesc g_" << domainCpp << "_vars[] = {\n";
        if (varCount == 0) {
            source << "    {nullptr, nullptr, nullptr},\n";
        } else {
            source << varEntries.str();
        }
        source << "};\n\n";
        source << "const size_t g_" << domainCpp << "_var_count = " << varCount
               << ";\n\n";

        source << "} // namespace app\n";

        std::filesystem::path hPath = outPath / ("domain_" + domainCpp + "_generated.h");
        std::filesystem::path cppPath = outPath / ("domain_" + domainCpp + "_generated.cpp");

        if (!WriteFile(hPath, header.str(), error)) return false;
        if (!WriteFile(cppPath, source.str(), error)) return false;
    }

    // Generate cross-domain bridge globals if any bridges exist.
    if (!graph_.GetBridges().empty()) {
        std::ostringstream bridgeHeader;
        bridgeHeader << "#pragma once\n\n";
        bridgeHeader << "// Generated by InverterCodegen. Do not edit by hand.\n\n";
        bridgeHeader << "#include \"InverterCodegen/RteQuantity.h\"\n\n";
        bridgeHeader << "namespace app {\n\n";
        for (const auto& bridge : graph_.GetBridges()) {
            const std::string bridgeName = "Bridge" + Capitalize(bridge.id);
            bridgeHeader << "struct " << bridgeName << "Type {\n";
            bridgeHeader << "    " << WireTypeToCpp(bridge.type) << " data;\n\n";
            bridgeHeader << "    void store(const " << WireTypeToCpp(bridge.type) << "& value);\n";
            bridgeHeader << "    " << WireTypeToCpp(bridge.type) << " load() const;\n";
            bridgeHeader << "};\n\n";
            bridgeHeader << "extern " << bridgeName << "Type " << bridgeName << ";\n\n";
        }
        bridgeHeader << "} // namespace app\n";

        std::ostringstream bridgeSource;
        bridgeSource << "// Generated by InverterCodegen. Do not edit by hand.\n\n";
        bridgeSource << "#include \"bridges_generated.h\"\n";
        bridgeSource << "#include \"platform_api.h\"\n\n";
        bridgeSource << "namespace app {\n\n";
        for (const auto& bridge : graph_.GetBridges()) {
            const std::string bridgeName = "Bridge" + Capitalize(bridge.id);
            bridgeSource << "void " << bridgeName << "Type::store(const " << WireTypeToCpp(bridge.type)
                         << "& value) {\n";
            bridgeSource << "    platform_critical_enter();\n";
            bridgeSource << "    data = value;\n";
            bridgeSource << "    platform_critical_exit();\n";
            bridgeSource << "}\n\n";
            bridgeSource << "" << WireTypeToCpp(bridge.type) << " " << bridgeName << "Type::load() const {\n";
            bridgeSource << "    " << WireTypeToCpp(bridge.type) << " result;\n";
            bridgeSource << "    platform_critical_enter();\n";
            bridgeSource << "    result = data;\n";
            bridgeSource << "    platform_critical_exit();\n";
            bridgeSource << "    return result;\n";
            bridgeSource << "}\n\n";
            bridgeSource << "" << bridgeName << "Type " << bridgeName << ";\n\n";
        }
        bridgeSource << "} // namespace app\n";

        if (!WriteFile(outPath / "bridges_generated.h", bridgeHeader.str(), error)) return false;
        if (!WriteFile(outPath / "bridges_generated.cpp", bridgeSource.str(), error)) return false;
    }

    return true;
}

}  // namespace InverterCodegen
