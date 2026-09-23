# MCP tool wishlist — lessons from debugging encoder angle glitches in FOC

Written after a live debugging session (2026-09-22): motor spun at 10 A via the
legacy `foc` path sounded rough. Root cause turned out to be intermittent
~30–40° forward steps in the encoder angle (visible in raw sin/cos and the
computed angle), each step kicking the Park transform (`id` spike + `iq` dip
~3×/second). These are the MCP capabilities that would have made the diagnosis
faster and more certain. Ordered by value.

## 1. High-rate burst capture

The decisive evidence came from the firmware's `enc_trace` console dump
(1024 samples @ ~1 kHz) — because the telemetry store samples at only ~85 Hz,
which aliases against a 36 Hz electrical frequency; angle-derived rates were
untrustworthy. Wanted: a generalized `rte_device_burst` — "return signals
X, Y, Z at the native ISR rate (or 5–20 kHz) for N hundred ms, from a
firmware/host ring buffer, as complete aligned samples." `rte_spike_capture`
already proves the pattern for currents; generalize it to arbitrary signals.

## 2. Triggered capture for intermittent glitches

The `id` spikes occurred ~3/s at random intervals; we had to run the motor
and hope an 8 s window caught enough. Wanted: "record at full rate when
`|foc_id| > 2.5`, keeping ±100 ms around each trigger." This is the exact
class of fault polling can't catch reliably.

## 3. Guaranteed-complete device dumps

`rte_device_command_response` is time-window observation; ~2/3 of the
`enc_trace` lines never arrived, forcing caveats in the analysis. Wanted: a
collect-until-end-marker mode with completeness reporting, or firmware-side
dump-to-buffer fetched as one structured blob.

## 4. True simultaneity in multi-signal capture

`rte_device_histories` states "individual timestamps; do not assume exact
simultaneity." Proving `id`-spike ↔ `iq`-dip ↔ angle-step coincidence needed
samples on the same tick. Wanted: snapshot-style multi-channel capture with a
single ADC/ISR timestamp base.

## 5. Spectral analysis in `rte_device_trends`

"Is the ripple periodic?" required exporting data and running a Python FFT.
Wanted: top-N FFT bins with magnitudes in the trends output, and ideally
cross-signal coherence (e.g., is `id` ripple phase-locked to `foc_elec_angle`?).

## 6. Units on legacy (non-graph) signals

`foc_*` signals are `observed_only` — no manifest units — so
"is `foc_elec_speed` rad/s, elec rpm, or mech rpm?" required reading
`FocController.cpp` to resolve. Wanted: unit metadata for firmware-published
signals, even a static table in the MCP server.

## 7. Gated-signal visibility

`EncoderSin`/`EncoderCos` reported `missing` during the FOC run because the
graph tim_isr domain is gated off while the legacy FOC owns the loop — the
exact data the investigation needed, silently unavailable. Wanted: a distinct
`state: "gated"` (with reason, e.g., "legacy FOC owns tim_isr") instead of
plain `missing`.

---

Related: `to_change.md` has the broader MCP improvement list (signal catalog,
build identity, grouped telemetry, snapshot bundles). Items #1 and #2 here
overlap its "triggered capture" entry; the rest are new.
