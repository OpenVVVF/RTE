# 2026-08-04 — Gemini 3.6 Flash: Windows fixes, audit fixes, FPGA FOC engine

## Goal (reconstructed from commits)

Continue the `feat/hostsim-live-telemetry` branch after Kimi's rebase: fix the
broken Windows build, apply audit findings, and start the FPGA FOC acceleration
track. (No session notes were left; this entry was written retrospectively by
Kimi on 2026-08-05 — Gemini, please add your own entry next time.)

## What was done

10 commits on `feat/hostsim-live-telemetry` (`dbcf05e..f4a55cd`):

- `dbcf05e` cleanup(HostSim,NodeGUI): cross-platform demo scripts and docs
- `3f27af0` fix(HostSim): don't kill NodeGUI from run_spwm_live.sh
- `9d124ed` fix(HostSim,NodeGUI): run loaded graph in simulator and add FOC runtime layout
- `b8bc0f8` fix(NodeGUI,NodeAPI): fix Windows build CMake duplicate target and unit test metadata assertions
- `c24ad57` fix(scripts): prepend Qt and MinGW bin paths to PATH in launchers to resolve Windows DLL entry point errors
- `5c5ef53` fix(audit): remove hardcoded SPWM fallback, add --templatesDir to launcher scripts, fix NodeGUI argument passing, dynamic pole pairs, class-based parameterInputs
- `a96c613` fix(templates,gui): zero-guards for division in templates; remove unused warning variable
- `cca2e73` fix(emitter): const-correctness + silence unused-variable warning in Emitter.cpp
- `0a6f923` fix(gui): enable native RTECodeEmitter code generation on Windows (Generate Code button)
- `f4a55cd` feat(fpga): Q15 fixed-point hardware FOC engine — `sincos_lut.v` (new),
  `foc_clarke_stub.v` / `foc_park_stub.v` / `foc_svpwm_stub.v` (now implemented, still
  named `_stub`), `Fpga/TangNano20k/tb/tb_foc.v`, `Fpga/TangNano20k/scripts/build_sim.ps1`,
  and MCU-side `Images/NucleoL476FW/.../Drivers/FPGA/FpgaSpiDriver.{h,cpp}` (SPI2 master).

Uncommitted WIP left behind (committed by Kimi as `d62b332`):
`Images/NucleoL476FW/svpwm_demo_graph.json` — new `Control.Ladrc` node
(first-order LADRC dq-current regulator, drop-in for `Control.Pi`) + node/port
descriptions.

## State at handoff (verified by Kimi 2026-08-05)

- Build: green (`ninja: no work to do`; NodeGUI.exe 2026-08-04 21:45).
- Tests: 82/82 ctest pass.
- The two build errors from Kimi's rebase verification (ConsolePanel qualified-id,
  HttpApiServer `arpa/inet.h` on Windows) are fixed — `HttpApiServer_win.cpp`
  platform split in `Source/NodeGUI/CMakeLists.txt:61-75`.

## Gotchas

- `foc_*_stub.v` files are **no longer stubs** — they contain the real Q15
  implementations. Rename planned (see `docs/ROADMAP_HYBRID_NUCLEO_FPGA.md`).
- FPGA register map (`Fpga/TangNano20k/docs/register_map.md`) is still v1
  (raw-duty PWM only) — the FOC engine is **not yet wired into the SPI register
  map or `tangnano20k_top.v`**.
- Branch was NOT pushed after these commits (fork still had pre-rebase history).
