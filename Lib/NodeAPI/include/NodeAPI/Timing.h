#pragma once

#include "NodeAPI/Graph.h"

#include <string>
#include <vector>

namespace NodeAPI::Timing {

struct ValidationResult {
    bool ok = true;
    std::vector<std::string> errors;

    void AddError(std::string message) {
        ok = false;
        errors.push_back(std::move(message));
    }
};

// Timing-domain validation for NodeAPI graphs.
//
// Every node carries a `domain` string (e.g. "isr_pwm", "adc_sample",
// "app_loop"). This validator enforces three rules:
//   1. The graph is a DAG — cycles are reported as errors.
//   2. Connections may only exist between nodes in the same timing domain.
//   3. Bridges may only connect nodes in different timing domains.
//
// Cross-domain data movement is represented by a `Bridge`, not by a direct
// `Connection`; the consuming project models explicit domain hand-offs with
// bridges and keeps intra-domain wiring with normal connections.
class Validator {
public:
    ValidationResult Validate(const Graph& graph) const;

private:
    ValidationResult CheckDomains(const Graph& graph) const;
    ValidationResult CheckEntryPoints(const Graph& graph) const;
    ValidationResult CheckCycles(const Graph& graph) const;
};

}  // namespace NodeAPI::Timing
