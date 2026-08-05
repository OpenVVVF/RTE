# rte-gateway

`rte-gateway` is the only process that opens the inverter serial device. It
parses telemetry once and serves NodeGUI, `rtectl`, and future integrations
through `/api/v1` HTTP endpoints and an SSE event stream.

## Run locally

```sh
./build/Source/RTEServer/rte-gateway --serial /dev/ttyACM0
./build/Source/RTECtl/rtectl info
./build/Source/RTECtl/rtectl control hold
```

The API binds to `127.0.0.1:18080` by default. To expose it to lab users,
place the host on the lab VPN and explicitly change `http.bind` in the JSON
configuration. There is intentionally no application authentication in this
release, so do not expose the port to an untrusted network.

For a system installation, copy `Deploy/rte-gateway.json` to
`/etc/rte/gateway.json`, install the binary as `/usr/local/bin/rte-gateway`,
create the dedicated `rte` account, and install `Deploy/rte-gateway.service`.

`RTEServer` remains as a warning-emitting executable alias for one release.
The old raw TCP serial bridge and unleased mutation routes have been removed.
Old read-only HTTP routes remain deprecated aliases during the transition.

## Client responsibility

Project graphs and builds remain local to NodeGUI/NodeAPI. Only the compiled
binary is uploaded. The gateway verifies its SHA-256 digest before queuing the
flash, so a remote flash uses exactly the bytes produced on the client.
