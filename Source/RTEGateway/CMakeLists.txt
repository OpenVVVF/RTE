add_executable(rte-gateway src/Main.cpp)

set_target_properties(rte-gateway PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(rte-gateway
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(rte-gateway
    PRIVATE
        RTE::Runtime
        InverterProtocol
        project_warnings
)

install(TARGETS rte-gateway RUNTIME DESTINATION bin)

add_test(NAME RTEGateway.ValidatesDeploymentConfig
    COMMAND rte-gateway
            --config ${CMAKE_SOURCE_DIR}/Deploy/rte-gateway.json
            --check-config
)
