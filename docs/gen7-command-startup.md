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
passed. The earlier processor safety synchronization image was flashed
before these startup changes; its live checks do not verify this fix.

## Connected-device deployment, 2026-10-09

Flashed and verified the G474 through USB DFU, then the generated H7 image
through the Gen7 bridge. Restarted the rebuilt Studio for the new serial
command prefix. Images from startup fix commit `289b716`:

- G474: `Images/Gen7FW/CoProcessor/build/Debug/CoProcessor.elf`, SHA-256
  `b84ec068012059a51a9c2bb60dd818c03d6711393a962b0d5bc2ec6369ad177d`.
- H7: `build/startup-command-fw/STM32CubeMX.elf`, SHA-256
  `862876774fe1f2513a60f2aac5d255cd4cd55102f7f43cfa6a4ab7a8b654748f`.

The first main flash attempt failed to activate the ROM bootloader while
Studio held the explicitly selected bridge port. Retrying with automatic
port discovery acquired Studio's flash handoff and succeeded. When Studio
owns the bridge, omit an explicit `serial` argument so the flash route can
acquire that handoff. The successful coprocessor flash was not repeated.

Live checks confirmed `foc_demo`, graph hash `bd5390d76c425eb1`, 82 nodes and
33 manifest signals. The three new shell diagnostic signals were discovered
and reported zero errors, drops, and rearm failures. Both current CLI mode
diagnosis and the RTE MCP reported a responding application; control-port
status showed `APP_WAIT=0`. G474 `UART_ERRORS=470` and `RX_DROPPED=313496`
remained unchanged across subsequent monitoring and the reboot check; these
cumulative values include the flash transitions and time without a reader.

Using the RTE MCP, sent `reboot` and immediately sent command discovery
(`help`) in the next call. The queued request returned all 53 commands with
the expected count, closing footer, and argument ranges. After reboot the
H7 shell diagnostic counters remained zero. This is one successful queued
command check on hardware, not a statistical test of intermittent cold boots.

With the bus supply off, telemetry read about 0.88 V. H7 still reported
Critical `Max22530Uv`; the G474 retained `PowerFeedbackLost` with trip reason
4 and inputs `09`, as in the earlier unpowered check. Generated and local
outputs were disabled, gate power was off, reset was asserted, and TIM1
measurement remained active. Faults were not cleared and motor control was
not started. Clean-state reboot fault behavior with the bus powered remains
unverified.
