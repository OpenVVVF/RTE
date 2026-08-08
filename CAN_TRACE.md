# High-rate CAN trace

This is a supplemental logging path. Existing UART telemetry and the CAN
session/command protocol continue to work normally.

## 1. Add trace nodes

Add one **High-rate Trace (8)** (`Debug.Trace8`) node to `tim_isr` and connect
all eight inputs. Set `Key0` through `Key7` to the CSV column names and
`Scale0` through `Scale7` to the engineering units per signed 16-bit count.
For example, `0.01` means 0.01 A/count for a current signal. Choose scales
that cover the expected ranges without clipping.

For setpoints or constants, use **Trace on Change** (`Debug.TraceEvent`) in
`app_loop`. Give each instance a unique `Channel` from 8 through 31. It sends
one initial snapshot and then transmits only when the value changes. Use a
cross-domain bridge if the source value belongs to an ISR domain.

## 2. Configure the firmware

A dedicated bus is recommended. For CAN A/FDCAN1 at 500 kbit/s nominal and
3 Mbit/s data:

```text
config set Can.A.En 1
config set Can.Trace.En 1
config set Can.Trace.Bus 1
config set Can.Trace.DataBitRate 3000000
config set Can.Trace.IdBase 1664
config set Can.Trace.AutoStart 0
```

These raw driver settings are saved immediately. Reboot so the selected CAN
controller is initialized for CAN-FD. `Can.Trace.Bus` uses `1` for CAN A and
`2` for CAN B.

## 3. Configure SocketCAN (Linux)

Replace `can0` if your adapter uses another interface:

```bash
sudo ip link set can0 down
sudo ip link set can0 type can bitrate 500000 dbitrate 3000000 fd on
sudo ip link set can0 up
```

The adapter must support CAN-FD and a 3 Mbit/s data phase.

## 4. Record and export

Start the recorder first so it receives the channel schemas:

```bash
rte trace record --interface can0 --output run.rtecap --seconds 10
```

While it is listening, start capture from the device UART console:

```text
trace start
```

After recording, export the portable capture file:

```bash
rte trace export --input run.rtecap --output run.csv
```

Use `--seconds 0` to record until Ctrl-C. Live recording currently requires
Linux SocketCAN; exporting `.rtecap` files works on Linux, Windows, and macOS.

Run `trace status` on the device and check that `dropped=0` and
`frame_drop=0`. Sample-sequence gaps and status rows are also retained in the
capture so data loss remains visible during analysis.
