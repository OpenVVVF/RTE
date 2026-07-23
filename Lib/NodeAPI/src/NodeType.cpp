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

}  // namespace NodeAPI
