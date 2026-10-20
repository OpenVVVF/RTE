// Build-time driver used by Lib/PlantCodegen/CMakeLists.txt to refresh the
// generated plant engines the golden tests link against.
//
// Usage: plant_codegen_gen <graph.json> <templatesDir> <outputDir>
// Exit codes follow the RTECLI convention: 0 ok / 2 usage / 3 load failed /
// 4 generation failed.

#include "PlantCodegen/PlantGenerator.h"

#include <cstdio>
#include <string>

#include "plant_test_util.h"

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr,
                     "usage: plant_codegen_gen <graph.json> <templatesDir> <outputDir>\n");
        return 2;
    }
    NodeAPI::Graph graph;
    std::string error;
    if (!planttest::LoadPlantGraph(argv[1], argv[2], graph, error)) {
        std::fprintf(stderr, "plant_codegen_gen: %s\n", error.c_str());
        return 3;
    }
    PlantCodegen::PlantGenerator generator(graph);
    if (!generator.Generate(argv[3], error)) {
        std::fprintf(stderr, "plant_codegen_gen: %s\n", error.c_str());
        return 4;
    }
    return 0;
}
