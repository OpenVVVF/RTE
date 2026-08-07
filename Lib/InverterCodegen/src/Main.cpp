#include <InverterCodegen/CodeGenerator.h>

#include <NodeAPI/NodeAPI.h>
#include <NodeAPI/Timing.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

// Reads the whole file. Returns false (with error set) when the file cannot
// be opened; an open-but-empty file returns true with empty content.
bool ReadFile(const std::string& path, std::string& content, std::string& error) {
    std::ifstream file(path);
    if (!file.is_open()) {
        error = "Failed to open input file: " + path;
        return false;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    content = buffer.str();
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <input_graph.json> <output_dir>\n";
        return 1;
    }

    const std::string inputPath = argv[1];
    const std::string outputDir = argv[2];

    std::string jsonText;
    std::string readError;
    if (!ReadFile(inputPath, jsonText, readError)) {
        std::cerr << readError << "\n";
        return 1;
    }
    if (jsonText.empty()) {
        std::cerr << "Input file is empty: " << inputPath << "\n";
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
