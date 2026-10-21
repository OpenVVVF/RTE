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

# Emit a plant engine for a graph at build time and wrap it as a static
# library compiled with project_warnings (a clean build here is also the
# zero-warnings proof for the emitted code). plantcodegen_golden_ex takes an
# explicit graph path, emit namespace and file stem (D1: distinct namespaces
# let several plants link into one binary); plantcodegen_golden keeps the v1
# defaults (namespace plant, plant_generated.h/.cpp) for compatibility.
set(PLANTGENCODE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(GLOB PLANT_TEMPLATE_FILES "${RTE_SOURCE_DIR}/Assets/PlantTemplates/*/*")

function(plantcodegen_golden_ex target graph_path emit_namespace file_stem)
    get_filename_component(graph_name "${graph_path}" NAME_WE)
    set(out_dir "${PLANTGENCODE_DIR}/${target}")
    add_custom_command(
        OUTPUT "${out_dir}/${file_stem}.cpp" "${out_dir}/${file_stem}.h"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${out_dir}"
        COMMAND plant_codegen_gen
                "${graph_path}"
                "${RTE_SOURCE_DIR}/Assets/PlantTemplates"
                "${out_dir}"
                "${emit_namespace}"
                "${file_stem}"
        DEPENDS plant_codegen_gen
                "${graph_path}"
                ${PLANT_TEMPLATE_FILES}
        VERBATIM
    )
    add_library(${target} STATIC "${out_dir}/${file_stem}.cpp")
    target_include_directories(${target} PUBLIC "${out_dir}")
    target_link_libraries(${target} PRIVATE project_warnings)
endfunction()

function(plantcodegen_golden target graph_name)
    plantcodegen_golden_ex(
        ${target}
        "${RTE_SOURCE_DIR}/Assets/PlantExamples/${graph_name}.json"
        plant
        plant_generated
    )
endfunction()

plantcodegen_golden(plant_golden_gen7 gen7_bench_pmsm)
# D1: the teco/zero goldens now get their own namespaces+stems so all golden
# plants can link into the single test binary together.
plantcodegen_golden_ex(plant_golden_teco
    "${RTE_SOURCE_DIR}/Assets/PlantExamples/teco_vhz_induction.json"
    plant_teco_vhz_induction teco_generated)
plantcodegen_golden_ex(plant_golden_zero
    "${RTE_SOURCE_DIR}/Assets/PlantExamples/zero_dsr_zf75_7.json"
    plant_zero_dsr_zf75_7 zero_generated)
# D1 dual-plant link case: the gen7 graph emitted twice into one binary.
plantcodegen_golden_ex(plant_golden_gen7_alt
    "${RTE_SOURCE_DIR}/Assets/PlantExamples/gen7_bench_pmsm.json"
    plant_gen7_bench_pmsm plant_alt_generated)
# CvClamp supply branch variant (regen into a 150 V clamp, 50 A clamp sink).
plantcodegen_golden_ex(plant_golden_cvclamp
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/gen7_cvclamp.json"
    plant_cvclamp cvclamp_generated)
# DC/DC converter-family goldens (D1 namespaces per graph; the rigs run no
# machine, the tests drive duty/phase through the PlantSetInput seam).
plantcodegen_golden_ex(plant_golden_buckboost
    "${RTE_SOURCE_DIR}/Assets/PlantExamples/buckboost_charger_example.json"
    plant_buckboost_charger_example buckboost_generated)
plantcodegen_golden_ex(plant_golden_dab
    "${RTE_SOURCE_DIR}/Assets/PlantExamples/dab_pack_example.json"
    plant_dab_pack_example dab_generated)

add_executable(PlantCodegen_tests
    tests/test_plant_generator.cpp
    tests/test_golden_gen7.cpp
    tests/test_dual_plant.cpp
    tests/test_golden_teco.cpp
    tests/test_golden_dcdd.cpp
    tests/test_plant_contract.cpp
)

target_link_libraries(PlantCodegen_tests
    PRIVATE PlantCodegenLib
            plant_golden_gen7
            plant_golden_gen7_alt
            plant_golden_teco
            plant_golden_zero
            plant_golden_cvclamp
            plant_golden_buckboost
            plant_golden_dab
            GTest::gtest_main
            project_warnings
)
target_compile_definitions(PlantCodegen_tests PRIVATE RTE_SOURCE_DIR="${RTE_SOURCE_DIR}")

gtest_discover_tests(PlantCodegen_tests)
