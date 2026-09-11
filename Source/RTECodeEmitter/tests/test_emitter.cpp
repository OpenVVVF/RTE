#include <gtest/gtest.h>

#include "../src/Emitter.h"
#include <RTELogger/Logger.h>

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

TEST(Emitter, CrossDomainBridgeGeneratesAtomicBridge) {
    const auto tempRoot = std::filesystem::temp_directory_path() / "rte_bridge_test";
    std::filesystem::remove_all(tempRoot);

    const auto baseSrc = tempRoot / "base";
    const auto graphPath = tempRoot / "graph.json";
    const auto outputDir = tempRoot / "out";

    WriteFile(baseSrc / "state.h",
              "#pragma once\n"
              "// RTE_EMIT: app_loop state\n"
              "// RTE_EMIT: tim_isr state\n"
              "struct AppState {\n"
              "    app::AppLoopState app_loop;\n"
              "    app::TimIsrState tim_isr;\n"
              "};\n");

    WriteFile(baseSrc / "main.cpp",
              "#include \"state.h\"\n"
              "AppState appState;\n"
              "void loop() {\n"
              "    // RTE_EMIT: app_loop step\n"
              "}\n");

    WriteFile(baseSrc / "isr.cpp",
              "#include \"state.h\"\n"
              "extern AppState appState;\n"
              "void isr() {\n"
              "    // RTE_EMIT: tim_isr step\n"
              "}\n");

    WriteFile(graphPath,
              "{\n"
              "  \"name\": \"bridge_test\",\n"
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
              "    },\n"
              "    {\n"
              "      \"id\": \"control.gain\",\n"
              "      \"displayName\": \"Gain\",\n"
              "      \"inputPorts\": [\n"
              "        {\"name\": \"in\", \"direction\": \"input\", \"type\": {\"quantity\": \"dimensionless\", \"frame\": \"scalar\", \"dtype\": \"f32\"}}\n"
              "      ],\n"
              "      \"outputPorts\": [\n"
              "        {\"name\": \"out\", \"direction\": \"output\", \"type\": {\"quantity\": \"dimensionless\", \"frame\": \"scalar\", \"dtype\": \"f32\"}}\n"
              "      ],\n"
              "      \"inlineCode\": \"out = gain * in;\",\n"
              "      \"constructorCode\": \"\",\n"
              "      \"classHeader\": \"\",\n"
              "      \"classDefinition\": \"\",\n"
              "      \"maxInstances\": 0,\n"
              "      \"isEntryPoint\": false\n"
              "    }\n"
              "  ],\n"
              "  \"nodes\": [\n"
              "    {\n"
              "      \"id\": \"throttle\",\n"
              "      \"type\": \"constant.value\",\n"
              "      \"displayName\": \"Throttle\",\n"
              "      \"domain\": \"app_loop\",\n"
              "      \"position\": {\"x\": 0.0, \"y\": 0.0},\n"
              "      \"parameters\": {\"value\": \"0.5\"}\n"
              "    },\n"
              "    {\n"
              "      \"id\": \"current_ref\",\n"
              "      \"type\": \"control.gain\",\n"
              "      \"displayName\": \"Current Ref\",\n"
              "      \"domain\": \"tim_isr\",\n"
              "      \"position\": {\"x\": 0.0, \"y\": 0.0},\n"
              "      \"parameters\": {\"gain\": \"2.0\"}\n"
              "    }\n"
              "  ],\n"
              "  \"connections\": [],\n"
              "  \"bridges\": [\n"
              "    {\n"
              "      \"id\": \"throttle_cmd\",\n"
              "      \"type\": {\"quantity\": \"dimensionless\", \"frame\": \"scalar\", \"dtype\": \"f32\"},\n"
              "      \"producer\": {\"nodeId\": \"throttle\", \"portName\": \"out\"},\n"
              "      \"consumer\": {\"nodeId\": \"current_ref\", \"portName\": \"in\"}\n"
              "    }\n"
              "  ]\n"
              "}\n");

    RTECodeEmitter::Logger logger(RTECodeEmitter::LogLevel::Error);
    RTECodeEmitter::Emitter emitter(logger);

    RTECodeEmitter::EmitterOptions options;
    options.baseSrc = baseSrc;
    options.graphPath = graphPath;
    options.outputDir = outputDir;
    options.verbosity = RTECodeEmitter::LogLevel::Error;

    ASSERT_TRUE(emitter.Run(options));

    const auto bridgesHeader = outputDir / "generated" / "bridges_generated.h";
    const auto appLoopSource = outputDir / "generated" / "domain_app_loop_generated.cpp";
    const auto timIsrSource = outputDir / "generated" / "domain_tim_isr_generated.cpp";

    EXPECT_TRUE(std::filesystem::exists(bridgesHeader));
    EXPECT_TRUE(std::filesystem::exists(appLoopSource));
    EXPECT_TRUE(std::filesystem::exists(timIsrSource));

    const std::string bridgesText = ReadFile(bridgesHeader);
    EXPECT_NE(bridgesText.find("extern BridgeThrottleCmdType BridgeThrottleCmd;"),
              std::string::npos);

    const std::string appLoopText = ReadFile(appLoopSource);
    EXPECT_NE(appLoopText.find("BridgeThrottleCmd.store(out);"),
              std::string::npos);

    const std::string timIsrText = ReadFile(timIsrSource);
    EXPECT_NE(timIsrText.find(
                  "const rte::Dimensionless in = BridgeThrottleCmd.load();"),
              std::string::npos);

    std::filesystem::remove_all(tempRoot);
}

TEST(Emitter, EmitsAuTypesForPhysicalPorts) {
    const auto tempRoot = std::filesystem::temp_directory_path() / "rte_au_types_test";
    std::filesystem::remove_all(tempRoot);

    const auto baseSrc = tempRoot / "base";
    const auto graphPath = tempRoot / "graph.json";
    const auto outputDir = tempRoot / "out";

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

    // Graph with a single node that has current and voltage outputs (no inputs
    // needed, so no connections are required).
    WriteFile(graphPath,
              "{\n"
              "  \"name\": \"au_test\",\n"
              "  \"nodeTypes\": [\n"
              "    {\n"
              "      \"id\": \"test.sources\",\n"
              "      \"displayName\": \"Sources\",\n"
              "      \"inputPorts\": [],\n"
              "      \"outputPorts\": [\n"
              "        {\"name\": \"i_out\", \"direction\": \"output\", \"type\": {\"quantity\": \"current\", \"frame\": \"scalar\", \"dtype\": \"f32\"}},\n"
              "        {\"name\": \"v_out\", \"direction\": \"output\", \"type\": {\"quantity\": \"voltage\", \"frame\": \"scalar\", \"dtype\": \"f32\"}}\n"
              "      ],\n"
              "      \"inlineCode\": \"i_out = rte::Amperes(1.0f); v_out = rte::Volts(2.0f);\"\n"
              "    }\n"
              "  ],\n"
              "  \"nodes\": [\n"
              "    {\n"
              "      \"id\": \"src\",\n"
              "      \"type\": \"test.sources\",\n"
              "      \"domain\": \"app_loop\",\n"
              "      \"position\": {\"x\": 0.0, \"y\": 0.0}\n"
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
    const auto generatedSource = outputDir / "generated" / "domain_app_loop_generated.cpp";

    const std::string headerText = ReadFile(generatedHeader);
    EXPECT_NE(headerText.find("#include \"InverterCodegen/RteQuantity.h\""), std::string::npos);

    const std::string sourceText = ReadFile(generatedSource);
    EXPECT_NE(sourceText.find("rte::Current& i_out"), std::string::npos);
    EXPECT_NE(sourceText.find("rte::Voltage& v_out"), std::string::npos);

    std::filesystem::remove_all(tempRoot);
}

TEST(Emitter, DryRunWithMissingOutputDirFailsCleanly) {
    /* Regression: with --dry-run the base copy is skipped, so the output
     * directory may not exist; scanning it used to throw
     * std::filesystem_error (SIGABRT). The emitter must fail cleanly. */
    const auto tempRoot = std::filesystem::temp_directory_path() / "rte_emitter_dryrun_test";
    std::filesystem::remove_all(tempRoot);

    const auto baseSrc = tempRoot / "base";
    const auto graphPath = tempRoot / "graph.json";

    WriteFile(baseSrc / "main.cpp",
              "void loop() {\n"
              "    // RTE_EMIT: app_loop step\n"
              "}\n");
    WriteFile(graphPath,
              "{\n"
              "  \"name\": \"dry_run\",\n"
              "  \"nodeTypes\": [],\n"
              "  \"nodes\": [],\n"
              "  \"connections\": []\n"
              "}\n");

    RTECodeEmitter::Logger logger(RTECodeEmitter::LogLevel::Error);
    RTECodeEmitter::Emitter emitter(logger);

    RTECodeEmitter::EmitterOptions options;
    options.baseSrc = baseSrc;
    options.graphPath = graphPath;
    options.outputDir = tempRoot / "output_does_not_exist";
    options.dryRun = true;
    options.verbosity = RTECodeEmitter::LogLevel::Error;

    EXPECT_FALSE(emitter.Run(options));  // clean failure, not a crash
    EXPECT_FALSE(std::filesystem::exists(options.outputDir));

    std::filesystem::remove_all(tempRoot);
}

TEST(Emitter, MarkerAboveIncludeBlockIsNotShifted) {
    /* The old splice logic shifted every marker by the number of inserted
     * include lines, assuming includes always land above all markers. A
     * marker ABOVE the include insertion point must not move. */
    const auto tempRoot = std::filesystem::temp_directory_path() / "rte_marker_shift_test";
    std::filesystem::remove_all(tempRoot);

    const auto baseSrc = tempRoot / "base";
    const auto graphPath = tempRoot / "graph.json";
    const auto outputDir = tempRoot / "out";

    // The state marker sits above the include block.
    WriteFile(baseSrc / "state.h",
              "// RTE_EMIT: app_loop state\n"
              "#pragma once\n"
              "\n"
              "#include <stdint.h>\n");
    WriteFile(baseSrc / "main.cpp",
              "#include \"state.h\"\n"
              "void loop() {\n"
              "    // RTE_EMIT: app_loop step\n"
              "}\n");
    WriteFile(graphPath,
              "{\n"
              "  \"name\": \"shift_test\",\n"
              "  \"nodeTypes\": [],\n"
              "  \"nodes\": [],\n"
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

    const std::string stateText = ReadFile(outputDir / "state.h");
    // The marker line itself was replaced...
    EXPECT_NE(stateText.find("namespace app {\n    struct AppLoopState;\n}"),
              std::string::npos);
    // ...and the include block below survived intact.
    EXPECT_NE(stateText.find("#pragma once"), std::string::npos);
    EXPECT_NE(stateText.find("#include <stdint.h>"), std::string::npos);
    EXPECT_NE(stateText.find("#include \"generated/domain_app_loop_generated.h\""),
              std::string::npos);

    const std::string mainText = ReadFile(outputDir / "main.cpp");
    EXPECT_NE(mainText.find("app::AppLoopStep(appState.app_loop);"), std::string::npos);

    std::filesystem::remove_all(tempRoot);
}

TEST(Emitter, PreservesCrlfLineEndings) {
    const auto tempRoot = std::filesystem::temp_directory_path() / "rte_crlf_test";
    std::filesystem::remove_all(tempRoot);

    const auto baseSrc = tempRoot / "base";
    const auto graphPath = tempRoot / "graph.json";
    const auto outputDir = tempRoot / "out";

    WriteFile(baseSrc / "state.h",
              "#pragma once\r\n"
              "// RTE_EMIT: app_loop state\r\n"
              "struct AppState {\r\n"
              "    app::AppLoopState app_loop;\r\n"
              "};\r\n");
    WriteFile(baseSrc / "main.cpp",
              "#include \"state.h\"\r\n"
              "AppState appState;\r\n"
              "void loop() {\r\n"
              "    // RTE_EMIT: app_loop step\r\n"
              "}\r\n");
    WriteFile(graphPath,
              "{\n"
              "  \"name\": \"crlf_test\",\n"
              "  \"nodeTypes\": [],\n"
              "  \"nodes\": [],\n"
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

    for (const auto& name : {"state.h", "main.cpp"}) {
        const std::string text = ReadFile(outputDir / name);
        // Strip all CRLF pairs; no bare '\n' or '\r' may remain.
        std::string stripped;
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
                ++i;
            } else {
                stripped += text[i];
            }
        }
        EXPECT_EQ(stripped.find('\n'), std::string::npos) << name;
        EXPECT_EQ(stripped.find('\r'), std::string::npos) << name;
    }

    const std::string mainText = ReadFile(outputDir / "main.cpp");
    EXPECT_NE(mainText.find("    app::AppLoopStep(appState.app_loop);\r\n"), std::string::npos);
    EXPECT_NE(mainText.find("#include \"generated/domain_app_loop_generated.h\"\r\n"),
              std::string::npos);

    std::filesystem::remove_all(tempRoot);
}

TEST(Emitter, LoadsTemplatesFromDirectory) {
    const auto tempRoot = std::filesystem::temp_directory_path() / "rte_templates_test";
    std::filesystem::remove_all(tempRoot);

    const auto baseSrc = tempRoot / "base";
    const auto templatesDir = tempRoot / "templates";
    const auto graphPath = tempRoot / "graph.json";
    const auto outputDir = tempRoot / "out";

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

    // Minimal folder template.
    std::filesystem::create_directories(templatesDir / "constant.value");
    WriteFile(templatesDir / "constant.value" / "node.json",
              "{\n"
              "  \"id\": \"constant.value\",\n"
              "  \"inputPorts\": [],\n"
              "  \"outputPorts\": [\n"
              "    {\"name\": \"out\", \"direction\": \"output\", \"type\": {\"quantity\": \"dimensionless\", \"frame\": \"scalar\", \"dtype\": \"f32\"}}\n"
              "  ],\n"
              "  \"parameterTypes\": {\"value\": {\"quantity\": \"dimensionless\", \"frame\": \"scalar\", \"dtype\": \"f32\"}}\n"
              "}\n");
    WriteFile(templatesDir / "constant.value" / "inline.cpp", "out = value;\n");

    // Graph references the template type but does not define it inline.
    WriteFile(graphPath,
              "{\n"
              "  \"name\": \"template_test\",\n"
              "  \"nodeTypes\": [],\n"
              "  \"nodes\": [\n"
              "    {\n"
              "      \"id\": \"constant\",\n"
              "      \"type\": \"constant.value\",\n"
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
    options.templatesDir = templatesDir;
    options.verbosity = RTECodeEmitter::LogLevel::Error;

    ASSERT_TRUE(emitter.Run(options));

    const auto generatedSource = outputDir / "generated" / "domain_app_loop_generated.cpp";
    ASSERT_TRUE(std::filesystem::exists(generatedSource));

    const std::string sourceText = ReadFile(generatedSource);
    EXPECT_NE(sourceText.find("Step node: constant (constant.value)"), std::string::npos);

    std::filesystem::remove_all(tempRoot);
}
