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
            return value;
    }
    // Unknown / framed: fall back to a plain float so existing templates keep
    // working, but this loses unit safety for those parameters.
    return value + "f";
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
            // a local variable declared by the template author.
            for (const auto& [key, value] : node->parameters) {
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
        header << "extern const RteParamDesc g_" << domainCpp << "_params[];\n";
        header << "extern const size_t g_" << domainCpp << "_param_count;\n";
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
                    auto paramType = nodeType->FindParameterType(key);
                    source << "        state." << node->id << "." << key << " = "
                           << (paramType ? ParameterValueToCpp(*paramType, value) : value + "f")
                           << ";\n";
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

            source << "    // Step node: " << node->id << " (" << nodeType->id << ")\n";
            source << "    {\n";

            // Inputs.
            for (const auto& port : nodeType->inputPorts) {
                auto src = FindSourceExpression(graph_, node->id, port.name);
                if (!src) {
                    error = "Input port '" + port.name + "' of node '" + node->id +
                            "' is not connected";
                    return false;
                }
                source << "        const " << WireTypeToCpp(port.type) << " " << port.name
                       << " = " << *src << ";\n";
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
                    source << "        Bridge" << Capitalize(bridge.id)
                           << ".store(" << port.name << ");\n";
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

            source << "    // Reset node: " << node->id << "\n";
            source << "    {\n";
            for (const auto& [key, value] : node->parameters) {
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

        // Parameter table: expose mutable scalar parameters for runtime command access.
        for (const auto& nodeId : order) {
            const auto node = graph_.FindNode(nodeId);
            if (!node) continue;
            const auto nodeType = graph_.FindNodeType(node->type);
            if (!nodeType) continue;

            const bool classBased = !nodeType->classHeader.empty() || !nodeType->classDefinition.empty();
            if (classBased) continue;

            for (const auto& [key, value] : node->parameters) {
                auto paramType = nodeType->FindParameterType(key);
                if (!paramType) continue;
                if (paramType->frame != NodeAPI::Frame::Scalar) continue;
                if (paramType->quantity == NodeAPI::Quantity::String) continue;
                if (paramType->quantity == NodeAPI::Quantity::Boolean) continue;

                const std::string setterName = "set_" + node->id + "_" + key;
                const std::string getterName = "get_" + node->id + "_" + key;
                const std::string cppType = WireTypeToCpp(*paramType);

                source << "static void " << setterName << "(void* state, float value) {\n";
                source << "    auto* s = static_cast<" << domainTitle << "State*>(state);\n";
                if (paramType->quantity == NodeAPI::Quantity::Current) {
                    source << "    s->" << node->id << "." << key << " = rte::Amperes(value);\n";
                } else if (paramType->quantity == NodeAPI::Quantity::Voltage) {
                    source << "    s->" << node->id << "." << key << " = rte::Volts(value);\n";
                } else {
                    source << "    s->" << node->id << "." << key << " = value;\n";
                }
                source << "}\n\n";

                source << "static float " << getterName << "(const void* state) {\n";
                source << "    const auto* s = static_cast<const " << domainTitle << "State*>(state);\n";
                if (paramType->quantity == NodeAPI::Quantity::Current) {
                    source << "    return s->" << node->id << "." << key << ".in(au::amperes);\n";
                } else if (paramType->quantity == NodeAPI::Quantity::Voltage) {
                    source << "    return s->" << node->id << "." << key << ".in(au::volts);\n";
                } else {
                    source << "    return s->" << node->id << "." << key << ";\n";
                }
                source << "}\n\n";
            }
        }

        source << "const RteParamDesc g_" << domainCpp << "_params[] = {\n";
        for (const auto& nodeId : order) {
            const auto node = graph_.FindNode(nodeId);
            if (!node) continue;
            const auto nodeType = graph_.FindNodeType(node->type);
            if (!nodeType) continue;

            const bool classBased = !nodeType->classHeader.empty() || !nodeType->classDefinition.empty();
            if (classBased) continue;

            for (const auto& [key, value] : node->parameters) {
                auto paramType = nodeType->FindParameterType(key);
                if (!paramType) continue;
                if (paramType->frame != NodeAPI::Frame::Scalar) continue;
                if (paramType->quantity == NodeAPI::Quantity::String) continue;
                if (paramType->quantity == NodeAPI::Quantity::Boolean) continue;

                const std::string paramName = node->id + "." + key;
                const std::string setterName = "set_" + node->id + "_" + key;
                const std::string getterName = "get_" + node->id + "_" + key;
                source << "    {\"" << paramName << "\", " << setterName << ", " << getterName << "},\n";
            }
        }
        source << "};\n\n";
        source << "const size_t g_" << domainCpp << "_param_count = "
               << "sizeof(g_" << domainCpp << "_params) / sizeof(g_" << domainCpp << "_params[0]);\n\n";

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
