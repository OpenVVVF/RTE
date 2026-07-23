#include <gtest/gtest.h>

#include "../src/Emitter.h"
#include "../src/Logger.h"

#include <filesystem>
#include <fstream>

namespace {

void WriteFile(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path);
    file << content;
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    return std::string((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
}

}  // namespace

TEST(Emitter, RejectsOutputInsideBaseSrc) {
    RTECodeEmitter::Logger logger(RTECodeEmitter::LogLevel::Error);
    RTECodeEmitter::Emitter emitter(logger);

    RTECodeEmitter::EmitterOptions options;
    options.baseSrc = "/tmp/rtest_base";
    options.graphPath = "/tmp/rtest_base/graph.json";
    options.outputDir = "/tmp/rtest_base/output";

    EXPECT_FALSE(emitter.Run(options));
}

TEST(Emitter, EndToEndGeneratesAndInserts) {
    const auto tempRoot = std::filesystem::temp_directory_path() / "rte_emitter_test";
    std::filesystem::remove_all(tempRoot);

    const auto baseSrc = tempRoot / "base";
    const auto graphPath = tempRoot / "graph.json";
    const auto outputDir = tempRoot / "out";

    // Create base firmware.
    WriteFile(baseSrc / "state.h",
              "#pragma once\n"
              "// RTE_EMIT: app_loop state\n"
              "struct AppState {\n"
              "    app::AppLoopState app_loop;\n"
              "};\n");

    WriteFile(baseSrc / "main.cpp",
              "#include \"state.h\"\n"
              "AppState appState;\n"
              "void loop() {\n"
              "    // RTE_EMIT: app_loop step\n"
              "}\n");

    // Create graph.
    WriteFile(graphPath,
              "{\n"
              "  \"name\": \"test\",\n"
              "  \"nodeTypes\": [\n"
              "    {\n"
              "      \"id\": \"constant.value\",\n"
              "      \"displayName\": \"Constant\",\n"
              "      \"inputPorts\": [],\n"
              "      \"outputPorts\": [\n"
              "        {\"name\": \"out\", \"direction\": \"output\", \"type\": {\"quantity\": \"dimensionless\", \"frame\": \"scalar\", \"dtype\": \"f32\"}}\n"
              "      ],\n"
              "      \"inlineCode\": \"out = value;\",\n"
              "      \"constructorCode\": \"\",\n"
              "      \"classHeader\": \"\",\n"
              "      \"classDefinition\": \"\",\n"
              "      \"maxInstances\": 0,\n"
              "      \"isEntryPoint\": false\n"
              "    }\n"
              "  ],\n"
              "  \"nodes\": [\n"
              "    {\n"
              "      \"id\": \"constant\",\n"
              "      \"type\": \"constant.value\",\n"
              "      \"displayName\": \"Constant\",\n"
              "      \"domain\": \"app_loop\",\n"
              "      \"position\": {\"x\": 0.0, \"y\": 0.0},\n"
              "      \"parameters\": {\"value\": \"0.5\"}\n"
              "    }\n"
              "  ],\n"
              "  \"connections\": []\n"
              "}\n");

    RTECodeEmitter::Logger logger(RTECodeEmitter::LogLevel::Error);
    RTECodeEmitter::Emitter emitter(logger);

    RTECodeEmitter::EmitterOptions options;
    options.baseSrc = baseSrc;
    options.graphPath = graphPath;
    options.outputDir = outputDir;
    options.verbosity = RTECodeEmitter::LogLevel::Error;

    ASSERT_TRUE(emitter.Run(options));

    const auto generatedHeader = outputDir / "generated" / "domain_app_loop_generated.h";
    const auto modifiedMain = outputDir / "main.cpp";
    const auto modifiedState = outputDir / "state.h";

    EXPECT_TRUE(std::filesystem::exists(generatedHeader));
    EXPECT_TRUE(std::filesystem::exists(modifiedMain));
    EXPECT_TRUE(std::filesystem::exists(modifiedState));

    const std::string mainText = ReadFile(modifiedMain);
    EXPECT_NE(mainText.find("app::AppLoopStep(appState.app_loop);"), std::string::npos);
    EXPECT_NE(mainText.find("#include \"generated/domain_app_loop_generated.h\""),
              std::string::npos);

    const std::string stateText = ReadFile(modifiedState);
    EXPECT_NE(stateText.find("namespace app"), std::string::npos);
    EXPECT_NE(stateText.find("struct AppLoopState;"), std::string::npos);

    std::filesystem::remove_all(tempRoot);
}
