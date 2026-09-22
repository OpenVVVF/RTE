# MCP agent workflow feedback — lessons from a drive-bringup session

Written after a live bring-up/debugging session (2026-09-22): Gen7 inverter on a
free-spinning dyno, chasing "FOC unstable at higher speeds" plus a string of
firmware bugs found along the way (vsense divider scale, resistance cal driving
the wrong bridge terminals, orphaned native FOC after `cal all`, missing
control-ownership interlock). This file is about **agent workflow ergonomics**
— round-trips, context cost, and safety rails — complementing the
diagnostics-focused wishlist in `mcp_debugging_wishlist.md` and the broader
list in `to_change.md`. Ordered by impact.

## 1. Correlated, blocking command responses

The console is a shared firehose: no request/response association, and lines
were observed being dropped between polls during verbose cal output — which
sent the investigation down a wrong path (a supposed flux-stage "race" that
was partly dropped console evidence). Wanted:

- Tag every line the firmware emits while executing a command (`[RSP:<id>]`),
  or move machine output to a framed channel (JSON lines) separate from the
  human console.
- `rte_device_command_response` should block until a terminator (line count,
  regex match, or idle timeout) instead of a fixed observation window; today
  the agent has to poll and guess completeness.

## 2. Compact / structured outputs

Context is the agent's budget. Examples from the session: every
`rte_device_trends` call returned chart text + full metrics JSON +
signal_status (~15–20k chars; >100k total across the session);
`rte_control_status` returns a ~60-entry age dict every call; `rte_flash`
returns raw STM32CubeProgrammer ANSI progress noise (the per-line truncation
note helps — summarize harder). Wanted: `fields` / `compact` / `format`
options on the heavy tools, structured error returns (the new control
interlock prints a refusal as console text an agent must parse), and batch
`config get` (list of keys in one call).

## 3. One-call health, preflight, and reset

Status was assembled manually dozens of times from three tools plus the
console. Wanted:

- `rte_device_health`: state, faults, foc_running, cal_active, vdc, gate
  readiness, stale/spoiled signals — one compact line.
- `rte_preflight`: pass/fail checks before `control start` (faults clear,
  native FOC idle, cal idle, vdc in range).
- `rte_device_reset(wait=true)`: reboot and block until the app is responsive.

Related firmware ask (biggest single workflow cost of the session): fix the
fault-latch bug — `clearfault` does not restore operation, so every trip
requires a reboot. ~12 reboots happened in one session because of it.

## 4. Structured fault record

Post-trip forensics meant reconstructing state from frozen telemetry.
Wanted: firmware keeps a small "last trip" record (id/iq/vd/vq/vdc/rpm/theta at
the fault instant) plus a `rte_faults` tool returning it as JSON.

## 5. High-rate capture that works over MCP

128 Hz telemetry cannot resolve a ~30 Hz current-loop oscillation — dynamics
had to be inferred from trends instead of measured. `rte_spike_capture`
**failed to parse a real capture** during the session (returned "no capture"
while the console showed one — concrete bug to fix). Bigger wish: a
generalized burst/triggered recorder (`rte_device_burst`: "signals X,Y,Z at
native ISR rate for N ms, one aligned timestamp base, fetched as a blob") —
`mcp_debugging_wishlist.md` items 1–4 cover this from the diagnostics angle;
see also the firmware `Trace8` CAN-FD plane, which has no fetch path today.

## 6. Structured cal results and progress

`cal all` prints a text summary; results were copy-parsed out of console
lines. Wanted: the FRAM motor profile (R/L/λ/offset/poles) as a JSON tool
(a `motorcfg dump` equivalent), and a cal-progress tool (stage + percent)
instead of polling verbose logs.

## 7. Config ergonomics

- `config set` persists KV keys immediately but graph-node keys are live-only
  until `config save` — the inconsistency is only discoverable from the
  echo text. Make it uniform or expose an explicit `persist:` argument.
- Annotate stored-vs-effective-vs-default in `config get`/`config list`
  (a cal writes the KV store directly; graph nodes keep boot-cached values
  until reboot — this trap cost real time).
- Checkpoint/restore full config to a host file around experiments (today:
  rely on reboots to undo live changes).
- `config get` prints 4 decimals: 93.6 uH displayed as `0.0001`.

## 8. Machine-readable hardware constants

Publish divider ratios, shunt values, and ADC full-scales in
`rte_build_info`'s manifest so agents can sanity-check scaling. The 1516-vs-1001
divider bug this session would have been flagged by a trivial
expected-vs-configured check.

## 9. Trustworthy simulation

`rte_sim` is marked not-correctly-implemented. Before graph edits (feedforward
wiring, compensation nodes), a sim that actually runs the control loop would
remove most reflash-and-pray cycles.

## 10. Small polish

- `rte_device_commands` returned "incomplete" on first call — auto-retry.
- An agent playbook in the repo docs: the fault-latch→reboot ritual,
  live-vs-saved config, reboot-wipes-live-values, spikes re-arm behavior, the
  native-FOC-vs-graph-control interlock (added this session), and the
  cal-writes-KV-but-graph-cache-needs-reboot trap.

## What already works well

`rte_build_info`'s manifest (graph hash + nodes + signals), signal
freshness/stopped states, trends oscillation detection, the terse
generate→build pipeline, `rte_flash`'s Studio coordination, graph validation,
external-writes gating, and the `AGENTS.md` / `MCP_AGENT_SETUP.md` docs —
they materially helped. The firmware-side pattern added this session (refuse
unsafe commands with a clear reason, e.g. `control start` vs native FOC) is
the right shape; extend it.
