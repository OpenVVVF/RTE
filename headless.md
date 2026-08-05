  ### 1. Build and test

  cmake -S . -B build
  cmake --build build --parallel
  ctest --test-dir build --output-on-failure

  Expected: all 91 tests pass.

  ### 2. Start the gateway on the headless machine

  Find the serial device:

  ls -l /dev/serial/by-id/

  For local-only testing:

  ./build/Source/RTEServer/rte-gateway \
    --serial /dev/ttyACM0 \
    --bind 127.0.0.1 \
    --http-port 18080

  For remote lab access:

  ./build/Source/RTEServer/rte-gateway \
    --serial /dev/ttyACM0 \
    --bind 0.0.0.0 \
    --http-port 18080

  Only expose 0.0.0.0 behind the lab VPN/firewall.

  ### 3. Test from another terminal or computer

  RTE_GATEWAY_URL=http://HEADLESS_MACHINE_IP:18080

  Check connectivity:

  ./build/Source/RTECtl/rtectl --server "$RTE_GATEWAY_URL" info
  ./build/Source/RTECtl/rtectl --server "$RTE_GATEWAY_URL" status

  Watch live telemetry and console output:

  ./build/Source/RTECtl/rtectl --server "$RTE_GATEWAY_URL" watch

  Print current console history:

  ./build/Source/RTECtl/rtectl --server "$RTE_GATEWAY_URL" console

  Send a harmless device command:

  ./build/Source/RTECtl/rtectl --server "$RTE_GATEWAY_URL" command help

  Replace help with a known safe read-only command if your firmware does not support it.

  ### 4. Test multiple clients and control locking

  On computer A:

  ./build/Source/RTECtl/rtectl --server "$RTE_GATEWAY_URL" control hold

  Leave it running. On computer B:

  ./build/Source/RTECtl/rtectl --server "$RTE_GATEWAY_URL" command help

  Computer B should report that another client holds control. Press Ctrl+C on computer A, then retry the command on B; it should succeed.

  You can also run watch simultaneously on several computers. All should receive the same telemetry.

  ### 5. Test remote NodeGUI

  On your workstation:

  ./build/Source/NodeGUI/NodeGUI --connect "$RTE_GATEWAY_URL"

  Verify:

  - Telemetry and console data appear.
  - NodeGUI initially says Observer.
  - Take Control enables commands and flashing.
  - A second NodeGUI shows who currently controls the gateway.

  ### 6. Test remote flashing

  This command actually rewrites the inverter firmware:

  ./build/Source/RTECtl/rtectl \
    --server "$RTE_GATEWAY_URL" \
    flash /absolute/path/to/firmware.bin

  Or build the graph locally and flash the resulting binary remotely:

  Tools/build_flash_graph.sh --flash-url "$RTE_GATEWAY_URL"

  For a build-only check first:

  Tools/build_flash_graph.sh --no-flash
  sha256sum build/foc-demo-fw/STM32CubeMX.bin

  The flash output should stream in the terminal and finish with Done. Afterward:

  ./build/Source/RTECtl/rtectl --server "$RTE_GATEWAY_URL" status
  ./build/Source/RTECtl/rtectl --server "$RTE_GATEWAY_URL" watch