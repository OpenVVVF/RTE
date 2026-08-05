add_executable(rtectl src/Main.cpp)
set_target_properties(rtectl PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_link_libraries(rtectl PRIVATE RTE::Runtime nlohmann_json project_warnings)

install(TARGETS rtectl RUNTIME DESTINATION bin)
