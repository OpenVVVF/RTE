#pragma once

#include "NodeAPI/Port.h"

#include <map>
#include <string>
#include <vector>

namespace NodeAPI {

struct Position {
    double x = 0.0;
    double y = 0.0;

    friend bool operator==(const Position& lhs, const Position& rhs) = default;
    friend bool operator!=(const Position& lhs, const Position& rhs) = default;
};

// An instance of a NodeType in the graph.
struct Node {
    std::string id;        // Unique within the graph.
    std::string type;      // References a NodeType::id.
    std::string displayName;
    std::string domain;    // Timing domain, e.g. "isr_pwm", "adc_sample", "app_loop".
    Position position;

    // Generic key/value parameters for this instance. These are passed into the
    // node's constructor/init by the consuming project's codegen.
    std::map<std::string, std::string> parameters;

    // Names of parameters to treat as input ports (wireable) instead of
    // constants.  A parameter listed here is bound from a connection like a
    // normal input port rather than from its value in `parameters`.
    std::vector<std::string> parameterInputs;

    // When true the node stays in the graph (and is shown by editors) but is
    // excluded from compilation: code emitters must skip it together with any
    // connections/bridges that touch it.
    bool excludeFromCompile = false;

    // When true the node and every node reachable from it through outgoing
    // connections/bridges (its "children") are excluded from compilation.
    bool excludeFromCompileRecursive = false;

    // Names of input ports (or parameterInputs) whose producer was excluded
    // from compilation. Emitters bind these to a zero "nothing" value instead
    // of erroring on the missing connection. Computed while pruning excluded
    // nodes; not serialized.
    std::vector<std::string> zeroInputs;

    friend bool operator==(const Node& lhs, const Node& rhs) = default;
    friend bool operator!=(const Node& lhs, const Node& rhs) = default;
};

}  // namespace NodeAPI
