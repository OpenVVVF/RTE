# 2026-08-05 — Code audit report (hardcoding / bad practices)

Audit by parallel explore subagents on post-rebase `feat/hostsim-live-telemetry`.
**Coverage: 5 of 9 modules.** Unaudited (agents died on quota): NodeGUI
`src/runtime`, NodeGUI core, `Images/*`, `Assets/*`.

Legend: 🔴 critical bug/risk · 🟡 hardcoding · 🔵 bad practice · ⚪ merge artifact/dead code

## Lib/InverterProtocol

- 🔴 `src/host/host_client.cpp:140` — `stats_.rx_bytes += n` without `stats_mtx_` (all
  neighboring writes take it; `stats()` reads under it). Data race / UB.
- 🔴 `src/host/host_client.cpp:47,51` — `sendCommandLine`/`sendPacket` unsynchronized
  against worker thread's `close()/open()/receivePacket()` on the same transport.
- 🔴 `src/host/tcp_transport.cpp:139-157` — `writeRaw` busy-spins with no timeout on
  persistent `WouldBlock`; dead-but-open peer wedges the caller forever.
- 🟡 `src/host/uart_transport.cpp:59,72,95-96` — `baud` parameter ignored (`(void)baud`),
  460800 hardcoded in 3 places despite `DEFAULT_BAUD` and plumbed baud arg. Silent API lie.
- 🟡 `src/host/host_client.cpp:56,61,71,77,122` — magic buffer sizes (16/32/64/4096);
  4096 duplicates `RX_FRAME_CAP` instead of referencing it.
- 🟡 `src/host/host_client.cpp:275,282` — magic key `"print"` hardcoded twice.
- 🟡 `src/host/tcp_transport.cpp:102` — IPv4 literals only (`inet_pton(AF_INET)`), no
  hostname resolution; fails silently for `localhost`.
- 🔵 `src/host/host_client.cpp:229-234` — UART reconnect logic is **dead**: `pumpTransport()`
  only sets `disconnect` for TCP; dropped serial spins counting `bad_frames` forever.
- 🔵 `src/host/tcp_transport.cpp:40,42` — `WSAStartup` return ignored; `WSACleanup` at
  static-destruction can race other statics' sockets.
- 🔵 `src/host/host_client.cpp:207` — `cb_stats_(stats_)` passes stats without mutex.
- 🔵 `tcp_transport.cpp:164-172` vs `uart_transport.cpp:180-188` — near-verbatim copy-paste
  (raw `new uint8_t[]` per send); same for `sendLine` pair.
- 🔵 `src/packet_builder.c:288` — memcpy of packed header assumes LE host + exact layout;
  no `static_assert(sizeof(ivp_header_t)==IVP_HEADER_SIZE)`.
- ⚪ `src/host/host_client.cpp:190-193` — empty `if` block + `last_good` feeding a no-op
  (merge leftover). `host_client.h:24` — `reject_decode` never incremented.
- Health: protocol C core is tight; host layer has the race, ignored baud, dead reconnect.

## Lib/InverterCodegen + Source/RTECodeEmitter

- 🔴 `Emitter.cpp:389,408,411,474`, `CodeGenerator.cpp:533` — throwing
  `create_directories` overload, no try/catch anywhere up to `main()`.
- 🔴 `Emitter.cpp:243-247,416-418` — `copy_file` return ignored; silently missing
  support headers → uncompilable firmware tree later.
- 🔴 `CodeGenerator.cpp:261-263` — Boolean params routed through float formatter;
  `"true"` would emit `true.0f`. Works only by undocumented storage convention.
- 🟡 `Source/RTECodeEmitter/CMakeLists.txt:8-9,31-32` — absolute build-machine paths
  baked into binary as compile definitions; breaks if repo moves. No runtime override.
- 🟡 `CodeGenerator.cpp:448`, `Emitter.cpp:95` — `chmod 0777` on generated files, no comment.
- 🟡 `CodeGenerator.cpp:276,400-402` — stringly-typed type-id prefixes (`config.`,
  `Values.Config`, …) — rename silently changes codegen.
- 🔵 `CodeGenerator.cpp:531-1056` — `Generate()` is one ~525-line function;
  `Emitter.cpp:259-685` — `Run()` ~425 lines. Maintainability hazard.
- 🔵 Duplicated helpers across the two modules: `SanitizeIdentifier`/`Capitalize`,
  `WriteFile`/`WriteFileText` + chmod/unlink workaround block.
- 🔵 `CodeGenerator.cpp:646,655,781,864` — fallback blindly appends `f` to unknown-typed
  param strings → silent garbage C++.
- 🔵 `Emitter.cpp:642-665` — marker silently dropped when `adjustedLine >= lines.size()`.
- ⚪ `Emitter.cpp:13 vs 21` duplicate `#include <cstring>`; `CodeGenerator.cpp:219-243`
  nested anonymous namespace (pasted-in block); `Lib/InverterCodegen/generated/`
  checked-in generator output.

## Lib/NodeAPI + Lib/RTELogger

- 🔴 `NodeAPI/src/Graph.cpp:190-199` — `Graph::Connect` never checks
  `ConsumerHasConnection(to)` → input port can accumulate multiple producers.
  `AddBridge` enforces the mirror invariant; strongly suggests forgotten check.
- 🔴 `RTELogger/src/Logger.cpp:21` — `std::localtime` shared static storage, not
  thread-safe; no locking in `Logger::Log`.
- 🟡 `NodeAPI/tests/test_templates.cpp:12-13` — test reaches outside lib dir into
  top-level `Assets/`; breaks standalone lib builds.
- 🔵 `NodeAPI/src/Serialization.cpp:304-317` — `LoadIntoGraph` discards bool results of
  Add*/Connect → graphs load "successfully" with items silently missing.
- 🔵 `NodeAPI/src/NodeTemplates.cpp:116-124` — doc says "every *.json in directory"
  but only subdirectories scanned; `filesLoaded` inflated; `ok=true` when zero types loaded.
- 🔵 `NodeAPI/src/NodeTemplates.cpp:66,105,116` — filesystem calls without `error_code`;
  `value("schemaVersion", 0)` can throw `type_error` outside try.
- 🔵 `NodeAPI/src/Graph.cpp:16-24` — `RemoveNodeType` leaves dangling instances.
- 🔵 `NodeAPI/src/NodeTemplates.cpp:76` — re-serializes + re-parses JSON to reuse an overload.
- 🔵 `RTELogger/include/.../Logger.h:6` — namespace is `RTECodeEmitter` (entrenched copy-paste legacy).
- ⚪ `Lib/NodeAPI/fw_test.bin` — committed 5-byte test artifact (`deadbeef42`).

## Source/RTEFirmwareBuilder

- 🔴 `src/Builder.cpp:284,296,47` — POSIX shell syntax (`export PATH=...;`, `popen`,
  `2>&1`, single-quote escaping) with no `_WIN32` guard; broken on Windows.
- 🔴 `Builder.cpp:29`, `Toolchain.cpp:18` — PATH split on `:` unconditionally
  (Windows uses `;`) → whole PATH treated as one dir.
- 🔴 `Builder.cpp:121` — throwing `remove_all(options.buildDir)` on user-supplied
  `--clean` path, no guard (wrong `--build-dir` = unguarded recursive delete).
- 🟡 `Builder.cpp:239,249-250` — hardcoded `STM32CubeMX` target/artifact names.
- 🟡 `Toolchain.cpp:51-60` — hardcoded `/usr/bin`, `/opt/gcc-arm-none-eabi/bin`, `/opt/st`.
- 🟡 `Toolchain.cpp:30` — magic `8` for root upward-walk depth.
- 🟡 `Builder.cpp:335,339` — assumes executable name `RTECodeEmitter`, no `.exe` handling.
- 🔵 `Builder.cpp:312` — `pclose` wait-status logged as "exit code" without WIFEXITED/WEXITSTATUS.
- 🔵 `Builder.cpp:22-35` vs `Toolchain.cpp:11-24` — verbatim duplicate PATH-split helper
  (including the `:` bug).
- Health: small readable module; POSIX assumptions are the one structural defect.

## Tools + root launchers

- 🔴 `migrate_graph_ids.py:191-197` — second `__main__` block shadows the first; active
  block calls `migrate(path)` **without** `templates_dir` → template canonicalization
  (`MERGE_TARGETS`, `load_template`) is dead when run as a script.
- 🔴 `can_session_client.py:90` — short START frame (dlc 1–2) → uncaught `struct.error`.
- 🔴 `can_session_client.py:106` — `decode_packet` unpacks `pkt[:16]` with no length check.
- 🟡 `launch_nodegui.bat:2`, `launch_svpwm_demo.bat:8`, `launch_svpwm_live.bat:2` —
  user-specific absolute path `C:\Users\bc200\.cursor\STMSTUFF` committed.
- 🟡 same files — TCP port `127.0.0.1:14608` hardcoded in 3 places.
- 🟡 `install_stm32_toolchain.sh:8` — `linux-x64` hardcoded, no platform guard.
- 🔵 `build_flash_foc_demo.sh` vs `build_flash_graph.sh` — near-total duplication
  (flash_state/flash_via_client/flash_firmware/arg parser); fix must be applied twice.
- 🔵 `fram_keys.py:189` — `f"{value:g}"` truncates precision on save→load round-trip.
- 🔵 `mcp2221a_gpio.py:172,183` — unpinned auto `pip install EasyMCP2221` + re-exec
  (supply-chain risk on hardware-control script); `:36` subprocess with no timeout;
  `:242-243` `cmd_release` swallows all exceptions, exits 0.
- 🔵 `sim_device.py:32-34` — POSIX-only, no guard; `:196-199` `--rate 0.5`→0 → ZeroDivisionError.
- ⚪ `migrate_graph_ids.py:13-51 vs 54-92` — `ID_MAP` defined twice, byte-identical
  (unresolved merge duplication). `Tools/tests/test_fram_keys.py:15` cwd-dependent sys.path.

## Recommendations (priority order for the hybrid pivot)

1. `host_client.cpp` stats race + dead UART reconnect — on the Nucleo telemetry path.
2. `Graph::Connect` consumer-occupancy check — affects every graph load.
3. `migrate_graph_ids.py` duplication — tool silently does half its job.
4. `launch_*.bat` absolute paths — replace with `%~dp0`-relative.
5. RTEFirmwareBuilder POSIX guards — needed when firmware builds move to Windows flow.
