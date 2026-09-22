# MCP improvements for fast, detailed agentic debugging

Target: make the RTE MCP server self-describing so an agent can debug FOC (or any
control problem) with minimal guessing and minimal round trips.

## 1. Signal catalog (biggest win)

Problem: today the agent infers structure from name prefixes (`cg_`, `thr_`,
`dclink_`, ...) and cannot tell a missing/unwired sensor from a nonexistent
signal. Example: `temp_motor_c: null` — unwired, unconfigured, or not in this
build?

Change:
- Add `rte_signal_info` (optionally filtered by group/prefix) returning per
  signal: name, group/node, unit, physical range, description, and state
  (`live` / `configured-not-streaming` / `missing`).
- Or embed the same metadata in the `rte_device_telemetry` response.

## 2. Firmware build identity

Problem: the agent cannot distinguish "controller producing wrong outputs" from
"this firmware doesn't contain the FOC node". It doesn't know what signals
*should* exist.

Change:
- Expose the active graph/build descriptor in `rte_device_status` (or a new
  `rte_build_info`): firmware name/version/hash, list of instantiated nodes
  with their exposed ports/signals.

## 3. Grouped telemetry

Problem: telemetry is a flat dict; ownership is name-convention-based and
breaks down when prefixes collide or are ambiguous.

Change: nest the `signals` object by node/group, mirroring the graph:
`{"CurrentGroup": {"iu_a": ..., "theta_rad": ...}, "Throttle": {...}}`.
Keep flat aliases for backward compatibility if needed.

## 4. Multi-signal aligned history

Problem: `rte_device_history` takes one signal. Correlating `iq_ref` vs
measured `iq` means N sequential calls with drifting time windows.

Change: accept a list of signals in one call and return samples with shared
timestamps (or per-sample dicts), so reference-vs-measured comparisons are
exact.

## 5. Named snapshot bundles

Problem: every debugging session re-derives the same "which signals do I need"
list, costing round trips.

Change: add pre-defined bundles, e.g. `bundle: "foc"` returning in one call:
controller state, fault flags/latches, id/iq ref + measured, vd/vq, PWM duty,
rotor angle, speed, vdc, inverter temps. Allow custom bundles by signal list
(builds on #4).

## 6. State and fault introspection

Problem: FOC debugging lives on state machine state, fault flags, and PWM
enable. These are either absent or hidden behind guessed string-signal names.

Change:
- Expose controller state, fault register (raw + decoded enum names), and
  gate/PWM enable as first-class telemetry signals or a `rte_status` tool.
- Decode enums to strings server-side (agent shouldn't bit-twiddle).
- Latch faults until explicitly read/cleared so transient faults aren't lost.

## 7. Triggered capture / debug sessions

Problem: history is passive post-hoc polling; interesting transients (a fault
trip, a current spike) happen between polls.

Change: a ring-buffer capture mode: `rte_capture_start(signals, rate, trigger)`
with triggers like `fault`, `signal > threshold`, or `manual`, and
`rte_capture_read` returning the aligned window around the trigger. This is
the single most valuable addition for intermittent FOC failures.

## 8. Consistency and ergonomics

- **Units/formatting**: keep units in names (already good); add scale/precision
  hints in the catalog so agents quote sensible values.
- **Error semantics**: consistent structured errors (`unknown_signal`,
  `not_in_build`, `transport_down`) instead of generic failures.
- **Rate control**: let the caller request history at reduced rate or with
  downsampling for long windows.
- **Link stats**: document what the counters mean in the response (or a
  `rte_link_info` tool). `reject_unknown_id` is benign noise; an agent
  shouldn't have to learn that by trial.
- **Batching**: tolerate batched tool calls without head-of-line blocking so
  an agent can fan out history queries in parallel.
- **Write safety**: for `rte_device_command`, return the echoed command plus a
  structured ack/nak; consider a confirmation gate on commands that enable
  switching or clear faults.

## Priority order if only a few land

1. Signal catalog / metadata in telemetry (#1)
2. Active graph/build node list (#2)
3. Multi-signal aligned history (#4)
4. Fault/state introspection (#6)
5. Triggered capture (#7) — highest value for intermittent failures, most work
