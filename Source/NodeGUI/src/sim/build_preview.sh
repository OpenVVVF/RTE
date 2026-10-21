#!/usr/bin/env bash
# Dev-only: builds the Simulate screen standalone preview (sim_preview into
# ${OUT:-/tmp/rte-sim-preview}) with raw g++ — the NodeGUI CMake target is
# owned by the parent lane, so this keeps the screen verifiable without
# touching any build files. Not shipped in the product build.
set -euo pipefail

RTE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
SRC="$RTE_ROOT/Source/NodeGUI/src"
QT="${QTDIR:-/home/tliao/qt/6.8.3/gcc_64}"
BUILD_GUI="${BUILD_GUI:-$RTE_ROOT/build-gui}"   # for libRTEAutomation.a + rte
OUT="${OUT:-/tmp/rte-sim-preview}"
RTE_BIN="${RTE_BIN:-$BUILD_GUI/bin/rte}"
WORK="${WORK:-/tmp/rte-sim-preview-obj}"
mkdir -p "$WORK"

MOC="$QT/libexec/moc"
INCLUDES=(
  -I"$SRC"
  -I"$RTE_ROOT/Source/RTEAutomation/include"
  -isystem "$QT/include"
  -isystem "$QT/include/QtCore"
  -isystem "$QT/include/QtGui"
  -isystem "$QT/include/QtWidgets"
  -isystem "$QT/include/QtOpenGL"
  -isystem "$QT/include/QtOpenGLWidgets"
)
DEFINES=(
  -DQT_CORE_LIB -DQT_GUI_LIB -DQT_WIDGETS_LIB -DQT_OPENGL_LIB
  -DQT_OPENGLWIDGETS_LIB -DQT_NO_DEBUG
  -DRTE_CLI_DEVELOPMENT_PATH="\"$RTE_BIN\""
  -DRTE_PROJECT_ROOT="\"$RTE_ROOT\""
  -DRTE_NODE_TEMPLATES_DIR="\"$RTE_ROOT/Assets/NodeTemplates\""
)
CXXFLAGS=(-std=c++20 -fPIC -Wall -Wextra -Wno-missing-field-initializers -O2)

# moc the QObject headers (mirrors what AUTOMOC does for the real target)
"$MOC" "$SRC/sim/SimulateScreen.h"   -o "$WORK/moc_SimulateScreen.cpp"
"$MOC" "$SRC/sim/SimTelemetryView.h" -o "$WORK/moc_SimTelemetryView.cpp"
"$MOC" "$SRC/runtime/SignalPlotWidget.h" -o "$WORK/moc_SignalPlotWidget.cpp"

SOURCES=(
  "$SRC/sim/SimulateScreen.cpp"
  "$SRC/sim/SimSession.cpp"
  "$SRC/sim/SimTelemetryView.cpp"
  "$SRC/sim/main_sim_preview_main.cpp"
  "$SRC/runtime/SignalPlotWidget.cpp"
  "$SRC/runtime/TelemetryStore.cpp"
  "$WORK/moc_SimulateScreen.cpp"
  "$WORK/moc_SimTelemetryView.cpp"
  "$WORK/moc_SignalPlotWidget.cpp"
)

OBJECTS=()
for src in "${SOURCES[@]}"; do
  obj="$WORK/$(basename "${src%.cpp}").o"
  /usr/bin/c++ "${DEFINES[@]}" "${INCLUDES[@]}" "${CXXFLAGS[@]}" -c "$src" -o "$obj"
  OBJECTS+=("$obj")
done

/usr/bin/c++ "${OBJECTS[@]}" \
  "$BUILD_GUI/lib/libRTEAutomation.a" \
  -L"$QT/lib" -Wl,-rpath,"$QT/lib" \
  -lQt6Widgets -lQt6Gui -lQt6Core -lQt6OpenGL -lQt6OpenGLWidgets -lGL \
  -o "$OUT"

echo "built: $OUT"
