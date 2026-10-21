add_library(RTESimLib STATIC
    src/Scenario.cpp
    src/SimRunner.cpp
)

target_include_directories(RTESimLib PUBLIC include)

# RTEAutomation brings the in-process emitter (Emitter.h), the process
# runner used for host compilation, and (transitively) NodeAPI.
target_link_libraries(RTESimLib
    PUBLIC RTEAutomation
    PRIVATE project_warnings
)

target_compile_definitions(RTESimLib
    PRIVATE RTESIM_RESOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

# The plant graph -> C++ step function is provided by the concurrently
# developed Lib/PlantCodegen; RTESim builds and runs without it (fallback
# plant + identical runtime ABI).
if(TARGET PlantCodegenLib)
    target_link_libraries(RTESimLib PRIVATE PlantCodegenLib)
    target_compile_definitions(RTESimLib PRIVATE RTESIM_HAVE_PLANTCODEGEN=1)
endif()

add_executable(RTESim_tests tests/test_sim_runner.cpp)
target_include_directories(RTESim_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(RTESim_tests
    PRIVATE RTESimLib GTest::gtest_main project_warnings
)
target_compile_definitions(RTESim_tests
    PRIVATE RTESIM_RESOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
# GoogleTest was only include()d inside Lib/NodeAPI's directory scope; without
# re-including here, gtest_discover_tests emits an empty discovery script path
# (-P "") and ctest reports RTESim_tests_NOT_BUILT.
include(GoogleTest)
gtest_discover_tests(RTESim_tests)

# ---------------------------------------------------------------------------
# F6: end-to-end gate — runs the real gen7 closed-loop smoke through the rte
# binary and asserts the session via twin.replay (see tests/e2e_smoke.py).
# Skip semantics: exit 77 with a printed reason when python3/twin or the rte
# binary is missing (SKIP_RETURN_CODE); the test never silently passes.
# ---------------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    # NOTE: $<TARGET_FILE:rte> resolves at generate time; Source/RTECLI is
    # add_subdirectory()d after this directory, so the rte target is not yet
    # in scope for an `if(TARGET rte)` check here. The e2e script itself
    # exits 77 (SKIP with reason) when the binary is absent at run time.
    add_test(NAME RTESim_e2e_smoke
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e_smoke.py
            --rte $<TARGET_FILE:rte>
            --ws ${CMAKE_SOURCE_DIR}/..
            --work-root ${CMAKE_CURRENT_BINARY_DIR}/e2e
    )
    set_tests_properties(RTESim_e2e_smoke PROPERTIES
        LABELS e2e
        SKIP_RETURN_CODE 77
        TIMEOUT 900
        ENVIRONMENT "PYTHONPATH=${CMAKE_SOURCE_DIR}/.."
    )
else()
    message(STATUS "RTESim_e2e_smoke NOT registered (no python3 interpreter found)")
endif()
