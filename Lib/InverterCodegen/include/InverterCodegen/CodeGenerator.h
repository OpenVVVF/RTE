#pragma once

#include <NodeAPI/Graph.h>

#include <string>

namespace InverterCodegen {

class CodeGenerator {
public:
    explicit CodeGenerator(const NodeAPI::Graph& graph);

    // Generates one header/source pair per timing domain into outputDir.
    // Returns true on success; on failure, error is filled.
    bool Generate(const std::string& outputDir, std::string& error) const;

private:
    const NodeAPI::Graph& graph_;
};

}  // namespace InverterCodegen
