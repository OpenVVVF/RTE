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
// C++ keyword and helper whitelist
// ---------------------------------------------------------------------------

const std::unordered_set<std::string>& CppKeywords() {
    static const std::unordered_set<std::string> kKeywords = {
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor",
        "bool", "break", "case", "catch", "char", "char8_t", "char16_t", "char32_t",
        "class", "compl", "concept", "const", "consteval", "constexpr", "constinit",
        "const_cast", "continue", "co_await", "co_return", "co_yield", "decltype",
        "default", "delete", "do", "double", "dynamic_cast", "else", "enum",
        "explicit", "export", "extern", "false", "float", "for", "friend", "goto",
        "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept",
        "not", "not_eq", "nullptr", "operator", "or", "or_eq", "private", "protected",
        "public", "register", "reinterpret_cast", "requires", "return", "short",
        "signed", "sizeof", "static", "static_assert", "static_cast", "struct",
        "switch", "template", "this", "thread_local", "throw", "true", "try",
        "typedef", "typeid", "typename", "union", "unsigned", "using", "virtual",
        "void", "volatile", "wchar_t", "while", "xor", "xor_eq"
    };
    return kKeywords;
}

const std::unordered_set<std::string>& BuiltinHelpers() {
    static const std::unordered_set<std::string> kHelpers = {
        "clamp", "min", "max", "abs", "fabs", "sin", "cos", "tan", "asin", "acos",
        "atan", "atan2", "sqrt", "cbrt", "pow", "exp", "log", "log10", "fmod",
        "round", "floor", "ceil", "trunc"
    };
    return kHelpers;
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

std::unordered_set<std::string> ExtractFunctionCallIdentifiers(const std::string& code) {
    std::unordered_set<std::string> result;
    // Find identifiers immediately followed by '('.
    std::regex re(R"(\b([A-Za-z_][A-Za-z0-9_]*)\s*\()");
    auto begin = std::sregex_iterator(code.begin(), code.end(), re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        result.insert(it->str(1));
    }
    return result;
}

std::unordered_set<std::string> ExtractMemberNames(
    const std::string& code,
    const std::string& objectName) {
    std::unordered_set<std::string> result;
    // Match objectName.member or objectName->member
    std::string pattern = R"(\b)" + objectName + R"((?:\.\s*|->\s*)([A-Za-z_][A-Za-z0-9_]*))";
    std::regex re(pattern);
    auto begin = std::sregex_iterator(code.begin(), code.end(), re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        result.insert(it->str(1));
    }
    return result;
}

std::unordered_set<std::string> ResolveIdentifiers(
    const std::string& code,
    const std::string& instanceName,
    bool isClassBased,
    std::string& /*error*/) {
    auto all = ExtractAllIdentifiers(code);
    auto memberNames = isClassBased ? ExtractMemberNames(code, instanceName)
                                    : std::unordered_set<std::string>{};
    auto funcCalls = ExtractFunctionCallIdentifiers(code);

    std::unordered_set<std::string> result;
    for (const auto& id : all) {
        if (CppKeywords().count(id)) continue;
        if (BuiltinHelpers().count(id)) continue;
        if (funcCalls.count(id)) continue;                 // function call
        if (isClassBased && id == instanceName) continue;  // resolved separately
        if (memberNames.count(id)) continue;               // instance.member
        result.insert(id);
    }
    return result;
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
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Type helpers (placeholder until real unit library is chosen)
// ---------------------------------------------------------------------------

std::string WireTypeToCpp(const NodeAPI::WireType& type) {
    // For now, map everything to float. Once the unit library is chosen,
    // this becomes: return "units::" + QuantityName(type.quantity) + FrameSuffix(type.frame);
    (void)type;
    return "float";
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

        source << "    struct " << node->id << "State {\n";

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
            // Function-style: parameters and inferred state members.
            auto ids = ResolveIdentifiers(nodeType->inlineCode, "instance", false, error);
            if (!error.empty()) return {};
            auto ctorIds = ResolveIdentifiers(nodeType->constructorCode, "instance", false, error);
            if (!error.empty()) return {};
            ids.insert(ctorIds.begin(), ctorIds.end());

            for (const auto& id : ids) {
                bool isInput = false, isOutput = false;
                for (const auto& p : nodeType->inputPorts) {
                    if (p.name == id) { isInput = true; break; }
                }
                for (const auto& p : nodeType->outputPorts) {
                    if (p.name == id) { isOutput = true; break; }
                }
                if (isInput || isOutput) continue;
                source << "        float " << id << ";\n";
            }
        }

        source << "    };\n";
        source << "\n";
        source << "    " << node->id << "State " << node->id << ";\n";
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
        header << "#include <stdint.h>\n\n";

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
        header << "\n} // namespace app\n";

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
                        source << "        const float " << key << " = " << value << "f;\n";
                    }
                }
            } else {
                for (const auto& [key, value] : node->parameters) {
                    source << "        state." << node->id << "." << key << " = " << value << "f;\n";
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
                        source << "        const float " << key << " = " << value << "f;\n";
                    }
                }
            } else {
                // Bind parameter/state refs.
                auto ids = ResolveIdentifiers(nodeType->inlineCode, "instance", false, error);
                if (!error.empty()) return false;

                for (const auto& id : ids) {
                    bool isInput = false, isOutput = false;
                    for (const auto& p : nodeType->inputPorts) {
                        if (p.name == id) { isInput = true; break; }
                    }
                    for (const auto& p : nodeType->outputPorts) {
                        if (p.name == id) { isOutput = true; break; }
                    }
                    if (isInput || isOutput) continue;
                    const bool isParam = node->parameters.count(id) > 0;
                    source << "        " << (isParam ? "const " : "") << "float& " << id
                           << " = state." << node->id << "." << id << ";\n";
                }
            }

            if (!nodeType->inlineCode.empty()) {
                source << "        " << nodeType->inlineCode << "\n";
            }

            source << "    }\n";
        }
        source << "}\n\n";
        source << "} // namespace app\n";

        std::filesystem::path hPath = outPath / ("domain_" + domainCpp + "_generated.h");
        std::filesystem::path cppPath = outPath / ("domain_" + domainCpp + "_generated.cpp");

        if (!WriteFile(hPath, header.str(), error)) return false;
        if (!WriteFile(cppPath, source.str(), error)) return false;
    }

    return true;
}

}  // namespace InverterCodegen
