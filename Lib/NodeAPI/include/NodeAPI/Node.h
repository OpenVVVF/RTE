#pragma once

#include "NodeAPI/Port.h"

#include <map>
#include <string>

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

    friend bool operator==(const Node& lhs, const Node& rhs) = default;
    friend bool operator!=(const Node& lhs, const Node& rhs) = default;
};

}  // namespace NodeAPI
