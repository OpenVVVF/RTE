#include "NodeAPI/NodeType.h"

namespace NodeAPI {

std::optional<Port> NodeType::FindInputPort(const std::string& name) const {
    for (const auto& port : inputPorts) {
        if (port.name == name) return port;
    }
    return std::nullopt;
}

std::optional<Port> NodeType::FindOutputPort(const std::string& name) const {
    for (const auto& port : outputPorts) {
        if (port.name == name) return port;
    }
    return std::nullopt;
}

std::optional<WireType> NodeType::FindParameterType(const std::string& name) const {
    auto it = parameterTypes.find(name);
    if (it != parameterTypes.end()) return it->second;
    return std::nullopt;
}

}  // namespace NodeAPI
