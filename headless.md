# Headless inverter access

The headless design has four deliberately separate components:

- **`RTERuntime`** is a Qt-free static library. It contains the HTTP/SSE API,
  telemetry store, API client, lease logic, and firmware upload support shared
  by the applications. It is an implementation library, not a daemon.
- **`rte-gateway`** is the only process allowed to open the inverter serial
  device. It parses each frame once and serves all clients over `/api/v1`.
- **`rtectl`** is the command-line API client used for diagnostics, automation,
  commands, and remote flashing.
- **`NodeGUI`** builds graphs and firmware locally, then observes or controls a
  local or remote gateway through the same `GatewayClient` library as `rtectl`.

There is no `RTEServer` executable and no raw serial-over-TCP bridge. A future
MCP server should be another client adapter over `GatewayClient`; it must not
open serial devices or be embedded in the gateway.

## Behavior guaranteed by the architecture

- Any number of NodeGUI/CLI observers can read telemetry and console output.
- One renewable operator lease gates commands and firmware uploads. A crashed
  controller loses the lease automatically; observers remain connected.
- SSE event IDs support ordered replay after short disconnects. A replay gap or
  gateway restart sends an explicit reset snapshot so plots do not connect
  unrelated time epochs.
- Telemetry is streamed at source cadence; text-heavy GUI presentation is
  throttled separately so console bursts do not stall graphs.
- Firmware is built on the client. The exact binary body and its SHA-256 digest
  are sent to the gateway, verified before staging, and flashed on the machine
  physically connected to the inverter.
- Gateway reachability and physical device connection are reported separately.
- Errors use JSON objects under `error` with a stable `code` and readable
  `message`.

The old `/api/info`, `/api/telemetry`, `/api/console`, and `/flash/status`
read-only routes remain as temporary migration aliases. Old mutation routes
cannot bypass leases and return HTTP 410. New clients must use `/api/v1`.

## Build and test

```bash
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The relevant outputs are:

```text
build/Source/RTEGateway/rte-gateway
build/Source/RTECtl/rtectl
build/Source/NodeGUI/NodeGUI
build/Lib/RTERuntime/libRTERuntime.a
```

## Run locally

Find a stable device path when available:

```bash
ls -l /dev/serial/by-id/
```

Start the gateway on loopback:

```bash
./build/Source/RTEGateway/rte-gateway \
  --serial /dev/ttyACM0 \
  --bind 127.0.0.1 \
  --http-port 18080
```

Then inspect it from another terminal:

```bash
./build/Source/RTECtl/rtectl info
./build/Source/RTECtl/rtectl status
./build/Source/RTECtl/rtectl watch
./build/Source/RTECtl/rtectl console
```

`status` shows the gateway connection, physical device connection, current
telemetry, lease owner, and flash state.

## Remote access

Prefer a VPN, SSH tunnel, or an authenticated reverse proxy. The gateway has no
application-level authentication, so do not bind it to an untrusted network.
An SSH tunnel keeps the service on loopback:

```bash
ssh -N -L 18080:127.0.0.1:18080 user@HEADLESS_HOST
```

Clients can then use the local tunnel:

```bash
./build/Source/RTECtl/rtectl --server http://127.0.0.1:18080 info
./build/Source/NodeGUI/NodeGUI --connect http://127.0.0.1:18080
```

On a trusted lab-only network, explicitly set `http.bind` to the host's lab
address. Avoid `0.0.0.0` unless a firewall or VPN limits who can reach it.

## Control and multiple clients

Hold control on computer A:

```bash
./build/Source/RTECtl/rtectl --server "$RTE_GATEWAY_URL" control hold
```

While that runs, `watch`, `console`, `info`, and `status` continue to work from
other clients. A command from computer B is rejected with the current lease
owner. Stop the holder with Ctrl+C and the lease is released; if the holder
disappears, it expires automatically.

```bash
./build/Source/RTECtl/rtectl --server "$RTE_GATEWAY_URL" command help
```

## Remote flashing

Build the graph and firmware on the workstation first:

```bash
Tools/build_flash_graph.sh --no-flash
sha256sum build/foc-demo-fw/STM32CubeMX.bin
```

The following command rewrites the connected inverter firmware:

```bash
./build/Source/RTECtl/rtectl \
  --server "$RTE_GATEWAY_URL" \
  flash /absolute/path/to/STM32CubeMX.bin
```

Or use the graph helper end to end:

```bash
Tools/build_flash_graph.sh --flash-url "$RTE_GATEWAY_URL"
```

The client acquires and renews control for the whole upload/flash operation,
prints the actual programmer output, and exits unsuccessfully if upload,
verification, GPIO boot control, or flashing fails.

## System service

Install the headless artifacts with the normal CMake prefix:

```bash
sudo cmake --install build --prefix /usr/local
sudo install -d -m 0755 /etc/rte
sudo install -m 0644 Deploy/rte-gateway.json /etc/rte/gateway.json
sudo install -m 0644 Deploy/rte-gateway.service \
  /etc/systemd/system/rte-gateway.service
```

Validate the deployed settings without touching the serial device:

```bash
/usr/local/bin/rte-gateway --config /etc/rte/gateway.json --check-config
```

Create the restricted service account once, adjust the serial path and tool
paths in `/etc/rte/gateway.json`, then enable the service:

```bash
sudo useradd --system --no-create-home --shell /usr/sbin/nologin rte
sudo usermod -a -G dialout rte
sudo systemctl daemon-reload
sudo systemctl enable --now rte-gateway
systemctl status rte-gateway
journalctl -u rte-gateway -f
```

The supplied unit runs as `rte`, restarts on failure, and applies filesystem and
privilege restrictions. Keep the JSON configuration readable only by trusted
administrators if the gateway later gains credentials or other secrets.
