#include "NodeAPI/Serialization.h"

#include <nlohmann/json.hpp>

namespace NodeAPI {

using json = nlohmann::json;

namespace {

json ToJson(const WireType& type) {
    return json::object({
        {"quantity", ToString(type.quantity)},
        {"frame", ToString(type.frame)},
        {"dtype", ToString(type.dtype)},
    });
}

WireType WireTypeFromJson(const json& j) {
    return WireType{
        .quantity = QuantityFromString(j.at("quantity").get<std::string>()),
        .frame = FrameFromString(j.at("frame").get<std::string>()),
        .dtype = DTypeFromString(j.at("dtype").get<std::string>()),
    };
}

json ToJson(const Port& port) {
    return json::object({
        {"name", port.name},
        {"description", port.description},
        {"direction", port.direction == PortDirection::Input ? "input" : "output"},
        {"type", ToJson(port.type)},
        {"optional", port.optional},
    });
}

Port PortFromJson(const json& j) {
    return Port{
        .name = j.at("name").get<std::string>(),
        .direction = j.at("direction").get<std::string>() == "input" ? PortDirection::Input
                                                                      : PortDirection::Output,
        .type = WireTypeFromJson(j.at("type")),
        .optional = j.value("optional", false),
        .description = j.value("description", ""),
    };
}

json ToJson(const Position& position) {
    return json::object({
        {"x", position.x},
        {"y", position.y},
    });
}

Position PositionFromJson(const json& j) {
    return Position{
        .x = j.at("x").get<double>(),
        .y = j.at("y").get<double>(),
    };
}

json ToJson(const NodeType& nodeType) {
    auto j = json::object({
        {"id", nodeType.id},
        {"displayName", nodeType.displayName},
        {"defaultName", nodeType.defaultName},
        {"description", nodeType.description},
        {"inlineCode", nodeType.inlineCode},
        {"constructorCode", nodeType.constructorCode},
        {"classHeader", nodeType.classHeader},
        {"classDefinition", nodeType.classDefinition},
        {"maxInstances", nodeType.maxInstances},
        {"isEntryPoint", nodeType.isEntryPoint},
        {"domain", nodeType.domain},
        {"inputPorts", json::array()},
        {"outputPorts", json::array()},
    });

    for (const auto& port : nodeType.inputPorts) {
        j["inputPorts"].push_back(ToJson(port));
    }
    for (const auto& port : nodeType.outputPorts) {
        j["outputPorts"].push_back(ToJson(port));
    }

    if (!nodeType.parameterTypes.empty()) {
        auto params = json::object();
        for (const auto& [key, type] : nodeType.parameterTypes) {
            params[key] = ToJson(type);
            if (const auto description = nodeType.FindParameterDescription(key)) {
                params[key]["description"] = *description;
            }
        }
        j["parameterTypes"] = params;
    }

    return j;
}

NodeType NodeTypeFromJson(const json& j) {
    NodeType nodeType;
    nodeType.id = j.at("id").get<std::string>();
    nodeType.displayName = j.value("displayName", "");
    nodeType.defaultName = j.value("defaultName", "");
    nodeType.description = j.value("description", "");
    nodeType.inlineCode = j.value("inlineCode", "");
    nodeType.constructorCode = j.value("constructorCode", "");
    nodeType.classHeader = j.value("classHeader", "");
    nodeType.classDefinition = j.value("classDefinition", "");
    nodeType.maxInstances = j.value("maxInstances", 0);
    nodeType.isEntryPoint = j.value("isEntryPoint", false);
    nodeType.domain = j.value("domain", "");

    for (const auto& item : j.at("inputPorts")) {
        nodeType.inputPorts.push_back(PortFromJson(item));
    }
    for (const auto& item : j.at("outputPorts")) {
        nodeType.outputPorts.push_back(PortFromJson(item));
    }

    if (j.contains("parameterTypes") && j.at("parameterTypes").is_object()) {
        for (const auto& [key, value] : j.at("parameterTypes").items()) {
            nodeType.parameterTypes[key] = WireTypeFromJson(value);
            const std::string description = value.value("description", "");
            if (!description.empty()) {
                nodeType.parameterDescriptions[key] = description;
            }
        }
    }

    return nodeType;
}

json ToJson(const Node& node) {
    auto params = json::object();
    for (const auto& [key, value] : node.parameters) {
        params[key] = value;
    }

    auto j = json::object({
        {"id", node.id},
        {"type", node.type},
        {"displayName", node.displayName},
        {"domain", node.domain},
        {"position", ToJson(node.position)},
        {"parameters", params},
    });
    if (!node.parameterInputs.empty()) {
        j["parameterInputs"] = node.parameterInputs;
    }
    if (node.excludeFromCompile) {
        j["excludeFromCompile"] = true;
    }
    if (node.excludeFromCompileRecursive) {
        j["excludeFromCompileRecursive"] = true;
    }
    return j;
}

Node NodeFromJson(const json& j) {
    Node node;
    node.id = j.at("id").get<std::string>();
    node.type = j.at("type").get<std::string>();
    node.displayName = j.value("displayName", "");
    node.domain = j.value("domain", "");
    node.position = PositionFromJson(j.at("position"));
    node.excludeFromCompile = j.value("excludeFromCompile", false);
    node.excludeFromCompileRecursive = j.value("excludeFromCompileRecursive", false);

    if (j.contains("parameters") && j.at("parameters").is_object()) {
        for (const auto& [key, value] : j.at("parameters").items()) {
            node.parameters[key] = value.get<std::string>();
        }
    }
    if (j.contains("parameterInputs") && j.at("parameterInputs").is_array()) {
        for (const auto& item : j.at("parameterInputs")) {
            node.parameterInputs.push_back(item.get<std::string>());
        }
    }

    return node;
}

json ToJson(const Connection& connection) {
    return json::object({
        {"id", connection.id},
        {"from", json::object({{"nodeId", connection.from.nodeId},
                                {"portName", connection.from.portName}})},
        {"to", json::object({{"nodeId", connection.to.nodeId},
                              {"portName", connection.to.portName}})},
    });
}

Connection ConnectionFromJson(const json& j) {
    return Connection{
        .id = j.at("id").get<std::string>(),
        .from = PortRef{
            .nodeId = j.at("from").at("nodeId").get<std::string>(),
            .portName = j.at("from").at("portName").get<std::string>(),
        },
        .to = PortRef{
            .nodeId = j.at("to").at("nodeId").get<std::string>(),
            .portName = j.at("to").at("portName").get<std::string>(),
        },
    };
}

json ToJson(const Bridge& bridge) {
    return json::object({
        {"id", bridge.id},
        {"type", ToJson(bridge.type)},
        {"producer", json::object({{"nodeId", bridge.producer.nodeId},
                                   {"portName", bridge.producer.portName}})},
        {"consumer", json::object({{"nodeId", bridge.consumer.nodeId},
                                   {"portName", bridge.consumer.portName}})},
    });
}

Bridge BridgeFromJson(const json& j) {
    return Bridge{
        .id = j.at("id").get<std::string>(),
        .type = WireTypeFromJson(j.at("type")),
        .producer = PortRef{
            .nodeId = j.at("producer").at("nodeId").get<std::string>(),
            .portName = j.at("producer").at("portName").get<std::string>(),
        },
        .consumer = PortRef{
            .nodeId = j.at("consumer").at("nodeId").get<std::string>(),
            .portName = j.at("consumer").at("portName").get<std::string>(),
        },
    };
}

}  // namespace

std::string SaveToJson(const Graph& graph) {
    json j;
    j["name"] = graph.GetName();
    j["nodeTypes"] = json::array();
    j["nodes"] = json::array();
    j["connections"] = json::array();
    j["bridges"] = json::array();

    for (const auto& nodeType : graph.GetNodeTypes()) {
        j["nodeTypes"].push_back(ToJson(nodeType));
    }
    for (const auto& node : graph.GetNodes()) {
        j["nodes"].push_back(ToJson(node));
    }
    for (const auto& connection : graph.GetConnections()) {
        j["connections"].push_back(ToJson(connection));
    }
    for (const auto& bridge : graph.GetBridges()) {
        j["bridges"].push_back(ToJson(bridge));
    }

    // Canonical output: keys sorted, arrays in graph order, trailing newline,
    // so repeated saves of an unchanged graph are byte-identical.
    return j.dump(2) + "\n";
}

Graph LoadFromJson(std::string_view jsonText) {
    Graph graph;
    LoadIntoGraph(graph, jsonText);
    return graph;
}

void LoadIntoGraph(Graph& graph, std::string_view jsonText) {
    const json j = json::parse(jsonText);

    graph.SetName(j.value("name", graph.GetName()));

    for (const auto& item : j.at("nodeTypes")) {
        graph.AddNodeType(NodeTypeFromJson(item));
    }
    for (const auto& item : j.at("nodes")) {
        graph.AddNode(NodeFromJson(item));
    }
    for (const auto& item : j.at("connections")) {
        graph.Connect(ConnectionFromJson(item));
    }
    if (j.contains("bridges")) {
        for (const auto& item : j.at("bridges")) {
            graph.AddBridge(BridgeFromJson(item));
        }
    }
}

NodeType NodeTypeFromJson(std::string_view jsonText) {
    return NodeTypeFromJson(json::parse(jsonText));
}

}  // namespace NodeAPI
