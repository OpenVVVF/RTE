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
// "app_loop"). This validator enforces two rules:
//   1. The graph is a DAG — cycles are reported as errors.
//   2. Connections may only exist between nodes in the same timing domain.
//
// Cross-domain data movement is intentionally not represented by a direct
// connection; the consuming project must model it explicitly (e.g. a global
// variable written in one domain and read in another).
class Validator {
public:
    ValidationResult Validate(const Graph& graph) const;

private:
    ValidationResult CheckDomains(const Graph& graph) const;
    ValidationResult CheckEntryPoints(const Graph& graph) const;
    ValidationResult CheckCycles(const Graph& graph) const;
};

}  // namespace NodeAPI::Timing
