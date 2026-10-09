# Gen7 startup command diagnosis and fix

## Findings

The reported symptom is an intermittently garbled or ignored first command,
probably after power-up or reboot. No failing hardware boot was captured;
electrical noise is a possible trigger, not an established root cause.
Source inspection identified concrete synchronization and recovery problems:

- The G474 released application commands on any zero byte or after 1.5 s,
  without checking that H7 startup had finished. Shell `reboot` did not
  restart that wait, so later queued commands could reach the resetting H7.
- H7 UART RX was enabled before the shell. Its final FIFO flush preceded
  command registration, allowing bytes or overrun flags to arrive during
  registration and affect the first command.
- H7 recovery discards queued bytes and ignores input until the next line
  delimiter. If recovery discarded the host's leading resynchronization
  newline, it could silently discard the following complete command too.
- A leading newline executes any existing partial input. Leftover text could
  become an unknown command, or a valid command lacking only its terminator
  could execute unexpectedly.
- Forwarding incomplete USB lines could leave the UART between line
  boundaries indefinitely, preventing insertion of private safety reports.

The software RX ring is explicitly reset at shell initialization. There is
no evidence that old software buffer contents survive reset as a command;
new bytes arriving around initialization and error recovery are the concerns.

## Changes

The G474 validates a bounded, complete IVP telemetry packet, including its
COBS framing, header, length, and CRC, before releasing application traffic.
H7 telemetry transmission runs in the application loop after shell and
hardware initialization. Physical reset excludes bytes received before NRST
release. Shell reboot first requires 20 ms of UART silence before accepting
new telemetry, excluding the previous application's in-flight frames. There
is no timed fallback that forwards into an unready application.

The H7 registers commands before its final atomic FIFO flush, error clear,
and RX arm. ASCII CAN (`0x18`) cancels pending input without executing it and
clears recovery discard state. Studio and the protocol library send CAN plus
newline before commands. The newline also permits older firmware to receive
the following command, though older shells may report the CAN line as an
unknown command. Bare terminal commands remain supported. Damaged commands
are discarded and reported, never automatically retried.

The G474 forwards only complete application lines, preserving room for
private safety status while a partial host line is staged. Overlong input is
discarded until a delimiter or explicit cancellation. ROM bootloader bytes
bypass shell staging and readiness checks.

Once per second, the H7 publishes `shell_uart_errors`, `shell_rx_dropped`,
and `shell_rx_rearm_failures`. Error recovery also emits a bounded diagnostic
requesting a resend. These counters describe reception on the H7; control
port `UART_ERRORS` and `RX_DROPPED` describe the opposite direction.
`rte device mode` / `rte_device_mode` expose `app_waiting` from control-port
`APP_WAIT`. A successful USB write does not establish command execution.

## Verification and deployment

Host regression tests exercise the real UART writer through a PTY and the
MCP/manual mode diagnosis. H7 tests link the real command shell and dispatcher
to verify cancellation, final initialization flush ordering, error recovery,
overlong input, ring overflow, and a failed initial RX arm.

G474 tests cover invalid CRC/noise, incomplete frames, frame overflow, quiet
intervals, millisecond rollover, and line staging across every ring offset.
A mocked hardware test executes the real USB bridge callbacks, main loop,
and DMA completion handlers: queued first commands stay held even after
3 s; valid telemetry releases safety status before host input; shell reboot
holds subsequent commands through old telemetry; incomplete USB input does
not block safety reports; USB BUSY or an absent host produces bounded drops;
ROM bootloader bytes still pass without a newline.

Built the G474 image, generated H7 `foc_demo` image, `rte`, and `RTEStudio`.
Both firmware regression suites and the required host integration tests
passed. **The startup fix has not been flashed or verified on hardware.**
Deploy the G474 first, then the generated H7 image, and restart the rebuilt
Studio. The earlier processor safety synchronization image was flashed
before these startup changes; its live checks do not verify this fix.
