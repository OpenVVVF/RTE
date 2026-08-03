#pragma once

#include "NodeAPI/Port.h"
#include "NodeAPI/WireType.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace NodeAPI {

// A node template stored in the project's node database.
//
// Each NodeType defines the interface (ports) and implementation pieces for a
// reusable node. Node instances reference a NodeType by its id and may override
// the display name and timing domain.
struct NodeType {
    std::string id;
    std::string displayName;
    std::string defaultName;  // Default instance name for GUI node creation.
    std::string description;
    std::vector<Port> inputPorts;
    std::vector<Port> outputPorts;

    // Per-parameter type information. Keys are parameter names that appear in
    // Node::parameters. Values tell the codegen which physical/unit type to use
    // when emitting constants or state members. Parameters not listed here are
    // treated as dimensionless floats by default.
    std::map<std::string, WireType> parameterTypes;
    std::map<std::string, std::string> parameterDescriptions;

    // Code pieces carried with the type (codegen-agnostic; the consuming project
    // decides how to interpret them).
    std::string inlineCode;        // e.g. per-step expression
    std::string constructorCode;   // e.g. constructor body / init code
    std::string classHeader;       // e.g. C++ class declaration
    std::string classDefinition;   // e.g. C++ class implementation

    // Runtime/usage constraints.
    std::size_t maxInstances = 0;  // 0 = unlimited, otherwise max number of instances.
    bool isEntryPoint = false;     // True if this node starts a new timing domain.
    std::string domain;            // If non-empty, instances are forced into this domain.

    std::optional<Port> FindInputPort(const std::string& name) const;
    std::optional<Port> FindOutputPort(const std::string& name) const;
    std::optional<WireType> FindParameterType(const std::string& name) const;
    std::optional<std::string> FindParameterDescription(const std::string& name) const;

    friend bool operator==(const NodeType& lhs, const NodeType& rhs) = default;
    friend bool operator!=(const NodeType& lhs, const NodeType& rhs) = default;
};

}  // namespace NodeAPI
