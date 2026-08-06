option(NODEGUI_ENABLE_FPS_OVERLAY "Show the FPS/frametime overlay in RTE Studio" ON)

add_executable(RTEStudio
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
    src/PreferencesDialog.cpp
    src/PreferencesDialog.h
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
    src/runtime/RuntimeSessionExporter.cpp
    src/runtime/RuntimeSessionExporter.h
    src/runtime/SignalPlotWidget.cpp
    src/runtime/SignalPlotWidget.h
    src/runtime/SignalTablePanel.cpp
    src/runtime/SignalTablePanel.h
    src/runtime/ConsolePanel.cpp
    src/runtime/ConsolePanel.h
    src/runtime/TelemetryPanel.cpp
    src/runtime/TelemetryPanel.h
    src/runtime/FlashPanel.cpp
    src/runtime/FlashPanel.h
    src/runtime/LocalSessionServer.cpp
    src/runtime/LocalSessionServer.h
    src/runtime/RuntimeTab.cpp
    src/runtime/RuntimeTab.h
)

set_target_properties(RTEStudio PROPERTIES
    OUTPUT_NAME rte-studio
    AUTOMOC ON
    AUTORCC ON
    AUTOUIC ON
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

if(APPLE)
    set_target_properties(RTEStudio PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "RTE Studio"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.openvvvf.rte-studio"
    )
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network OpenGL OpenGLWidgets)

set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/QtNodes EXCLUDE_FROM_ALL)

target_compile_definitions(RTEStudio
    PRIVATE
        RTE_PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
        RTE_NODE_TEMPLATES_DIR="${CMAKE_SOURCE_DIR}/Assets/NodeTemplates"
        RTE_TOOLS_DIR="${CMAKE_SOURCE_DIR}/Tools"
        RTE_CLI_DEVELOPMENT_PATH="$<TARGET_FILE:rte>"
)

if(NODEGUI_ENABLE_FPS_OVERLAY)
    target_compile_definitions(RTEStudio PRIVATE NODEGUI_ENABLE_FPS_OVERLAY=1)
endif()


target_include_directories(RTEStudio
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/QtNodes/include/QtNodes/internal
)

target_link_libraries(RTEStudio
    PRIVATE
        NodeAPI
        RTEAutomation
        InverterProtocol
        QtNodes::QtNodes
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Network
        Qt6::OpenGL
        Qt6::OpenGLWidgets
        project_warnings
)

add_dependencies(RTEStudio rte)

# One-cycle developer compatibility for scripts that still build the old
# target name. The installed executable is only rte-studio.
add_custom_target(NodeGUI DEPENDS RTEStudio)

install(TARGETS RTEStudio
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    BUNDLE DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Qt's deployment helper copies the required Qt libraries and platform
# plugins into the install tree on Linux, Windows, and macOS. This keeps the
# GitHub Actions archives runnable without a system Qt installation.
qt_generate_deploy_app_script(
    TARGET RTEStudio
    OUTPUT_SCRIPT rte_studio_deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${rte_studio_deploy_script})
