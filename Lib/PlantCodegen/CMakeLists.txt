add_library(PlantCodegenLib STATIC
    src/PlantGenerator.cpp
)

target_include_directories(PlantCodegenLib
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(PlantCodegenLib
    PUBLIC NodeAPI
    PRIVATE project_warnings
)

# Small driver used at build time: refreshes the checked-into-the-build-tree
# generated plant engines the golden tests link against (the tests themselves
# load the same JSON in-process through the public API).
add_executable(plant_codegen_gen tests/gen_main.cpp)
target_link_libraries(plant_codegen_gen PRIVATE PlantCodegenLib project_warnings)
target_compile_definitions(plant_codegen_gen PRIVATE RTE_SOURCE_DIR="${RTE_SOURCE_DIR}")

include(GoogleTest)

# Emit plant_generated.{h,cpp} for a PlantExamples graph at build time and
# wrap it as a static library compiled with project_warnings (a clean build
# here is also the zero-warnings proof for the emitted code).
set(PLANTGENCODE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(GLOB PLANT_TEMPLATE_FILES "${RTE_SOURCE_DIR}/Assets/PlantTemplates/*/*")

function(plantcodegen_golden target graph_name)
    set(out_dir "${PLANTGENCODE_DIR}/${graph_name}")
    add_custom_command(
        OUTPUT "${out_dir}/plant_generated.cpp" "${out_dir}/plant_generated.h"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${out_dir}"
        COMMAND plant_codegen_gen
                "${RTE_SOURCE_DIR}/Assets/PlantExamples/${graph_name}.json"
                "${RTE_SOURCE_DIR}/Assets/PlantTemplates"
                "${out_dir}"
        DEPENDS plant_codegen_gen
                "${RTE_SOURCE_DIR}/Assets/PlantExamples/${graph_name}.json"
                ${PLANT_TEMPLATE_FILES}
        VERBATIM
    )
    add_library(${target} STATIC "${out_dir}/plant_generated.cpp")
    target_include_directories(${target} PUBLIC "${out_dir}")
    target_link_libraries(${target} PRIVATE project_warnings)
endfunction()

plantcodegen_golden(plant_golden_gen7 gen7_bench_pmsm)
plantcodegen_golden(plant_golden_teco teco_vhz_induction)
plantcodegen_golden(plant_golden_zero zero_dsr_zf75_7)

add_executable(PlantCodegen_tests
    tests/test_plant_generator.cpp
    tests/test_golden_gen7.cpp
)

target_link_libraries(PlantCodegen_tests
    PRIVATE PlantCodegenLib plant_golden_gen7 GTest::gtest_main project_warnings
)
target_compile_definitions(PlantCodegen_tests PRIVATE RTE_SOURCE_DIR="${RTE_SOURCE_DIR}")

gtest_discover_tests(PlantCodegen_tests)
