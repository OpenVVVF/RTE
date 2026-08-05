add_executable(rte-gateway src/Main.cpp)
add_executable(RTEServer src/Main.cpp)
target_compile_definitions(RTEServer PRIVATE RTE_DEPRECATED_SERVER_ALIAS=1)

set_target_properties(rte-gateway RTEServer PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(rte-gateway
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_include_directories(RTEServer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(rte-gateway
    PRIVATE
        RTEShared
        InverterProtocol
        project_warnings
)
target_link_libraries(RTEServer PRIVATE RTEShared InverterProtocol project_warnings)
