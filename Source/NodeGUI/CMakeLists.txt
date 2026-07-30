option(NODEGUI_ENABLE_FPS_OVERLAY "Show the FPS/frametime overlay in NodeGUI" ON)

add_executable(NodeGUI
    src/Main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/GraphScene.cpp
    src/GraphScene.h
    src/GraphView.cpp
    src/GraphView.h
    src/NodeDataModel.cpp
    src/NodeDataModel.h
    src/NodeGraphModel.cpp
    src/NodeGraphModel.h
    src/BridgeConnectionPainter.cpp
    src/BridgeConnectionPainter.h
    src/TypedNodePainter.cpp
    src/TypedNodePainter.h
    src/ParameterBlock.cpp
    src/ParameterBlock.h
    src/ParameterNodeGeometry.cpp
    src/ParameterNodeGeometry.h
    src/NodePalette.cpp
    src/NodePalette.h
    src/InspectorPanel.cpp
    src/InspectorPanel.h
    src/PortStyle.cpp
    src/PortStyle.h
    src/FrameRateMonitor.cpp
    src/FrameRateMonitor.h
    src/runtime/TelemetryStore.cpp
    src/runtime/TelemetryStore.h
    src/runtime/LegacyTelemetryClient.cpp
    src/runtime/LegacyTelemetryClient.h
    src/runtime/RuntimeController.cpp
    src/runtime/RuntimeController.h
    src/runtime/SignalPlotWidget.cpp
    src/runtime/SignalPlotWidget.h
    src/runtime/SignalTablePanel.cpp
    src/runtime/SignalTablePanel.h
    src/runtime/ConsolePanel.cpp
    src/runtime/ConsolePanel.h
    src/runtime/SimSpeedControl.cpp
    src/runtime/SimSpeedControl.h
    src/runtime/TelemetryPanel.cpp
    src/runtime/TelemetryPanel.h
    src/runtime/FlashPanel.cpp
    src/runtime/FlashPanel.h
    src/runtime/RuntimeTab.cpp
    src/runtime/RuntimeTab.h
)

if(WIN32)
    target_sources(NodeGUI PRIVATE
        src/runtime/FirmwareUpdater_win.cpp
        src/runtime/FirmwareUpdater.h
        src/runtime/HttpApiServer_win.cpp
        src/runtime/HttpApiServer.h
    )
else()
    target_sources(NodeGUI PRIVATE
        src/runtime/FirmwareUpdater.cpp
        src/runtime/FirmwareUpdater.h
        src/runtime/HttpApiServer.cpp
        src/runtime/HttpApiServer.h
    )
endif()

set_target_properties(NodeGUI PROPERTIES
    AUTOMOC ON
    AUTORCC ON
    AUTOUIC ON
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets)

set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/QtNodes)

target_compile_definitions(NodeGUI
    PRIVATE
        RTE_PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
        RTE_NODE_TEMPLATES_DIR="${CMAKE_SOURCE_DIR}/Assets/NodeTemplates"
        RTE_TOOLS_DIR="${CMAKE_SOURCE_DIR}/Tools"
)

if(NODEGUI_ENABLE_FPS_OVERLAY)
    target_compile_definitions(NodeGUI PRIVATE NODEGUI_ENABLE_FPS_OVERLAY=1)
endif()


target_include_directories(NodeGUI
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/QtNodes/include/QtNodes/internal
)

target_link_libraries(NodeGUI
    PRIVATE
        NodeAPI
        InverterProtocol
        QtNodes::QtNodes
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::OpenGL
        Qt6::OpenGLWidgets
        project_warnings
)
