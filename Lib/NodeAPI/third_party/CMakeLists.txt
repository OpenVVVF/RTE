# Minimal third-party glue for NodeAPI.

# googletest (provides GTest::gtest_main).
set(INSTALL_GTEST OFF)
set(gtest_force_shared_crt ON)
add_subdirectory(googletest)

# nlohmann_json is header-only in single_include/.
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/nlohmann_json/single_include)
target_compile_features(nlohmann_json INTERFACE cxx_std_20)
