#pragma once

#include "NodeAPI/WireType.h"

#include <string>

namespace NodeAPI {

enum class PortDirection {
    Input,
    Output,
};

struct Port {
    std::string name;
    PortDirection direction = PortDirection::Input;
    WireType type;

    friend bool operator==(const Port& lhs, const Port& rhs) = default;
    friend bool operator!=(const Port& lhs, const Port& rhs) = default;
};

struct PortRef {
    std::string nodeId;
    std::string portName;

    friend bool operator==(const PortRef& lhs, const PortRef& rhs) = default;
    friend bool operator!=(const PortRef& lhs, const PortRef& rhs) = default;
    friend bool operator<(const PortRef& lhs, const PortRef& rhs) {
        return lhs.nodeId < rhs.nodeId ||
               (lhs.nodeId == rhs.nodeId && lhs.portName < rhs.portName);
    }
};

}  // namespace NodeAPI
