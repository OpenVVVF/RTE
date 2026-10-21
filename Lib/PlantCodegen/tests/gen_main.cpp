// Build-time driver used by Lib/PlantCodegen/CMakeLists.txt to refresh the
// generated plant engines the golden tests link against.
//
// Usage: plant_codegen_gen <graph.json> <templatesDir> <outputDir>
//                          [emitNamespace] [fileStem]
// The optional emitNamespace/fileStem implement D1 (two plants in one binary
// need distinct namespaces/file names); omitted they keep the v1 defaults
// (namespace plant, plant_generated.h/.cpp).
// Exit codes follow the RTECLI convention: 0 ok / 2 usage / 3 load failed /
// 4 generation failed.

#include "PlantCodegen/PlantGenerator.h"

#include <cstdio>
#include <string>
#include <vector>

#include "plant_test_util.h"

int main(int argc, char** argv) {
    if (argc < 4 || argc > 6) {
        std::fprintf(stderr,
                     "usage: plant_codegen_gen <graph.json> <templatesDir> <outputDir>"
                     " [emitNamespace] [fileStem]\n");
        return 2;
    }
    NodeAPI::Graph graph;
    std::string error;
    if (!planttest::LoadPlantGraph(argv[1], argv[2], graph, error)) {
        std::fprintf(stderr, "plant_codegen_gen: %s\n", error.c_str());
        return 3;
    }
    PlantCodegen::PlantGenerator::Options opts;
    if (argc >= 5) opts.emitNamespace = argv[4];
    if (argc >= 6) opts.fileStem = argv[5];
    PlantCodegen::PlantGenerator generator(graph, opts);
    std::vector<std::string> warnings;
    if (!generator.Generate(argv[3], error, &warnings)) {
        std::fprintf(stderr, "plant_codegen_gen: %s\n", error.c_str());
        return 4;
    }
    for (const auto& warning : warnings) {  // D2: mild issues, still visible
        std::fprintf(stderr, "plant_codegen_gen: warning: %s\n", warning.c_str());
    }
    return 0;
}
