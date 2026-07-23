#include <InverterCodegen/CodeGenerator.h>

#include <NodeAPI/NodeAPI.h>
#include <NodeAPI/Timing.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

std::string ReadFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return {};
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <input_graph.json> <output_dir>\n";
        return 1;
    }

    const std::string inputPath = argv[1];
    const std::string outputDir = argv[2];

    const std::string jsonText = ReadFile(inputPath);
    if (jsonText.empty()) {
        std::cerr << "Failed to read input file: " << inputPath << "\n";
        return 1;
    }

    NodeAPI::Graph graph;
    try {
        graph = NodeAPI::LoadFromJson(jsonText);
    } catch (const std::exception& e) {
        std::cerr << "Failed to parse graph JSON: " << e.what() << "\n";
        return 1;
    }

    NodeAPI::Timing::Validator validator;
    auto timingResult = validator.Validate(graph);
    if (!timingResult.ok) {
        std::cerr << "Timing validation failed:\n";
        for (const auto& err : timingResult.errors) {
            std::cerr << "  - " << err << "\n";
        }
        return 1;
    }

    InverterCodegen::CodeGenerator generator(graph);
    std::string error;
    if (!generator.Generate(outputDir, error)) {
        std::cerr << "Code generation failed: " << error << "\n";
        return 1;
    }

    std::cout << "Generated domain files in " << outputDir << "\n";
    return 0;
}
