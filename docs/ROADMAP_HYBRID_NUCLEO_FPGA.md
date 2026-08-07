# Roadmap — Hybrid Nucleo + FPGA control platform

**Status:** fork direction (hibyemy/RTE), decided 2026-08-05. Doc-only so far —
no Phase 1–3 code has been written.

## Direction

The project's primary target pivots from **simulation-first** (HostSim /
ngspice plant co-simulation) to a **hybrid hardware controller**:

- **Nucleo-L476RG** (STM32L476, Cortex-M4F) — runs the RTE-generated graph:
  outer control loops, telemetry (IVP over UART/CAN), FRAM KV config, safety
  supervision.
- **Tang Nano 20K** (Gowin GW2AR-LV18) — offloads the time-critical inner FOC
  path as a Q15 fixed-point hardware engine: sin/cos LUT, Clarke, Park, SVPWM,
  complementary PWM with dead-time. MCU talks to it as an SPI slave (SPI2).
- **HostSim / ngspice** become *development aids only*: useful for graph
  bring-up and regression, but **maintenance-only** — no new simulation
  features unless they directly serve hybrid bring-up (e.g. a simulated FPGA
  peer so graphs can be exercised before the board arrives).

Rationale: simulation proved the graph/codegen/telemetry stack; the differentiating
work now is deterministic inner-loop latency and hardware offload, which
simulation cannot validate.

## Existing assets (what we build on)

| Asset | Path | State |
|-------|------|-------|
| SPI register map v1 (frozen contract) | `Fpga/TangNano20k/docs/register_map.md` | Raw-duty PWM only; `VERSION=0x0001` |
| PWM peripheral RTL | `Fpga/TangNano20k/rtl/{spi_regs,pwm_complementary,deadtime_pair,tangnano20k_top}.v` | Implemented, testbenched (`tb_pwm.v`, `tb_spi.v`) |
| Q15 FOC engine | `Fpga/TangNano20k/rtl/{sincos_lut,foc_clarke_stub,foc_park_stub,foc_svpwm_stub}.v` + `tb/tb_foc.v` | Implemented (Gemini, `f4a55cd`) but **not wired into top/SPI**; files still named `_stub` |
| MCU SPI2 driver | `Images/NucleoL476FW/.../Drivers/FPGA/FpgaSpiDriver.{h,cpp}` | Basic register read/write |
| Nucleo base image | `Images/NucleoL476FW/` (`feat/nucleo-l476-base-image`, PR #2) | Bring-up platform, MERGEABLE |
| Gowin project | `Fpga/TangNano20k/scripts/gowin_project.tcl`, `flash_notes.md` | Vendor-toolchain flow (chosen: Gowin IDE) |
| Sim testbenches | `Fpga/TangNano20k/scripts/build_sim.{sh,ps1}` (iverilog) | Runnable on Windows + Linux |
| Telemetry stack | IVP over UART/CAN/TCP + NodeGUI runtime panels | Live (PR #3) |
| LADRC regulator node | `Control.Ladrc` in Nucleo SPWM demo graph (`d62b332`) | Candidate outer-loop pairing with FPGA inner loop |

## Phase 1 — FPGA contract + RTL integration

Goal: FOC engine reachable from the MCU through the SPI register map.

1. **Register map v2** (`register_map.md`, bump `VERSION` → `0x0002`):
   - `CTRL.FOC_EN` — mode select: raw-duty (v1 behavior) vs FOC engine drives SVPWM.
   - `ID_SP`, `IQ_SP` — dq current setpoints, Q15 signed.
   - `IA_FB`, `IB_FB`, `THETA_EL` (or `SPEED_EL`) — feedback inputs written by MCU
     until on-FPGA ADC/encoder exists.
   - `DUTY_U/V/W` become RO readback of engine output in FOC mode.
   - `FAULT` bits: engine saturation, over-modulation, setpoint-while-disabled.
   - Keep v1 addresses stable (append-only) — the map is a frozen contract.
2. **Rename** `foc_*_stub.v` → `foc_*.v` (`git mv`; update `gowin_project.tcl`,
   `build_sim.*`, `tb_foc.v`).
3. **Integrate** engine into `tangnano20k_top.v` behind `CTRL.FOC_EN`; mux engine
   duties vs register duties into `pwm_complementary`.
4. **Verify**: extend `tb_foc.v` (setpoint step, saturation, fault W1C) and
   `tb_spi.v` (FOC register access); run both `build_sim` scripts.

## Phase 2 — MCU driver + codegen

Goal: a graph node can target the FPGA engine, with sim fallback.

1. Extend `FpgaSpiDriver` with the FOC API (`SetCurrentSetpoints(id, iq)`,
   `SetFeedback(ia, ib, theta)`, `ReadDuties`, fault clear/poll), respecting the
   v2 map; MAGIC/VERSION probe at init with graceful degradation to raw-duty mode.
2. Platform hooks in `Images/NucleoL476FW` (`platform_api` impl) so generated
   code can call the driver.
3. New node template (working name `Control.FpgaFoc`) emitting driver calls on
   the Nucleo platform; HostSim backend keeps the graph runnable in simulation
   (software FOC fallback path).
4. Fix audit findings that sit on the hybrid path (see
   `agent-log/2026-08-05-audit-report.md`):
   - `host_client.cpp:140` stats race + `:229-234` dead UART reconnect (telemetry).
   - `Graph::Connect` missing consumer-occupancy check (every graph load).
   - `migrate_graph_ids.py` duplicated `ID_MAP` / shadowed `__main__`.

## Phase 3 — Tooling + hardware bring-up

Toolchain decision: **Gowin IDE** (vendor flow; `gowin_project.tcl` already
exists). Open-source yosys/nextpnr/apicula flow documented as an alternative
only if a contributor requests it.

1. Scripted bitstream build via Gowin CLI (`gw_sh gowin_project.tcl`) +
   openFPGALoader flash script under `Tools/`.
2. HIL bring-up checklist (documented, executed on bench):
   MAGIC probe → SCRATCH loopback → raw-duty PWM on scope (dead-time verified)
   → FOC mode open-loop spin → closed-loop with `Control.Ladrc` outer loop.
3. NodeGUI flash-panel awareness of the FPGA target (deferred until Phase 1–2 land).
4. Update `Fpga/TangNano20k/scripts/flash_notes.md` as real flashing happens.

## Cross-cutting

- **Agent continuity:** every LLM session logs to `agent-log/` (see its README).
- **Upstream relationship:** OpenVVVF/RTE continues its own telemetry/firmware
  work; keep rebasing this fork regularly. Hybrid-FPGA work is fork-local for
  now; consider upstreaming the register-map contract once v2 is proven on hardware.
- **Open PRs:** #2 (Nucleo base image — mergeable) is the foundation for Phase 2;
  #3 (HostSim telemetry — conflicting after rebase) provides the telemetry path
  used for hybrid bring-up observability.

## Open questions

- On-FPGA current/angle sensing (ADC interface, encoder/QEP module) vs
  MCU-written feedback registers for v2 — v2 assumes MCU-written feedback.
- Q15 vs Q4.12/Q2.14 for the engine datapath — validate dynamic range against
  the Nucleo demo motor before freezing v2 map field widths.
- SPI bandwidth at 10 kHz inner loop: setpoint+feedback frame budget vs the
  recommended ≤10 MHz SCLK — measure during Phase 3 bring-up.
