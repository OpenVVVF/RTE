#!/usr/bin/env python3
"""F6 end-to-end gate for rte sim (ctest name: RTESim_e2e_smoke, label e2e).

Runs the gen7 closed-loop smoke through the real rte binary (controller
emission + plant codegen + host compile + run), loads the v3 JSONL through
twin.replay, and asserts the load-bearing channels:

  * exit 0; >250 telemetry records;
  * cg_iq_a settled mean in [40,60] over the smoke's settle window
    (IqVar=150 slew trajectory at 500 rpm — the 3 s card settles ~46 A;
    measured on this build: see the e2e_gen7 numbers in the run log);
  * vdc_v in [143,147] (gen7 bench supply minus series-R drop);
  * tw_p_heat_w > 100 under load (real diag power off the graph, NOT zero;
    idle frames correctly read 0 — F16 open-gate idle physics, no idle draw);
  * temp_inv1_c finite; dclink_p_w present and nonconstant;
  * EncoderSin inside the healthy learned bounds;
  * tw_p_dc_w + tw_p_shaft_em_w present and nonzero under load;
  * second phase: fault-inject OpenGates collapses |cg_iq_a| < 2 A and the
    clear recovers back onto the pre-inject trajectory.

Exit 77 = SKIP (python/twin or the rte binary missing) — ctest maps that
through SKIP_RETURN_CODE; we never "pass" by being absent.
"""

import argparse
import math
import os
import subprocess
import sys

SKIP = 77

# Phase 1 = the decided 3 s gen7 smoke (reset/start/IqVar=150/500 rpm);
# phase 2 = OpenGates inject + clear, appended in the same run.
GEN7_EVENTS = [
    "t=0.1 kind=reset",
    "t=0.5 kind=start",
    "t=1.0 kind=var-set arg=IqVar=150",
    "t=3.2 kind=fault-inject arg=OpenGates",
    "t=3.55 kind=fault-clear arg=OpenGates",
]


def fail(msg):
    print(f"e2e FAIL: {msg}")
    sys.exit(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rte", required=True, help="path to the rte binary")
    ap.add_argument("--ws", required=True, help="openvvvf workspace root")
    ap.add_argument("--work-root", required=True)
    args = ap.parse_args()

    rte = os.path.abspath(args.rte)
    ws = os.path.abspath(args.ws)
    if not (os.path.isfile(rte) and os.access(rte, os.X_OK)):
        print(f"SKIP: rte binary missing: {rte}")
        return SKIP
    sys.path.insert(0, ws)
    os.environ["PYTHONPATH"] = ws + os.pathsep + os.environ.get("PYTHONPATH", "")
    try:
        from twin.replay import load_session
    except Exception as e:  # noqa: BLE001
        print(f"SKIP: twin not importable from {ws}: {e}")
        return SKIP

    os.makedirs(args.work_root, exist_ok=True)
    out = os.path.join(args.work_root, "e2e_gen7.jsonl")
    cmd = [rte, "sim",
           "--sw-graph", os.path.join(ws, "RTE/Assets/Examples/foc_demo.json"),
           "--plant-graph", os.path.join(ws, "RTE/Assets/PlantExamples/gen7_bench_pmsm.json"),
           "--base-source", os.path.join(ws, "RTE/Images/Gen6FW"),
           "--name", "e2e_gen7",
           "--duration", "4.0",
           "--dyno-rpm", "500",
           "--out", out]
    for ev in GEN7_EVENTS:
        cmd += ["--event", ev]
    proc = subprocess.run(cmd, capture_output=True, text=True, cwd=ws,
                          timeout=900)
    sys.stderr.write(proc.stderr)
    if proc.returncode != 0:
        fail(f"rte sim exited {proc.returncode}\n{proc.stdout}\n{proc.stderr}")
    if not os.path.isfile(out):
        fail(f"session not produced: {out}")

    s = load_session(out)
    frames = s.telemetry
    if len(frames) <= 250:
        fail(f"telemetry records {len(frames)} <= 250")

    # provenance stamp (F15)
    plant = s.meta.get("plant")
    if plant not in ("generated", "fallback"):
        fail(f"session_start plant stamp missing/bad: {plant!r}")

    def series(key):
        return s.series(key)

    def mean(vals):
        return sum(vals) / max(1, len(vals))

    def window(key, lo, hi):
        return [v for (t, v) in series(key) if lo <= t < hi]

    # cg_iq_a settled 40..60 A mean over the 3 s smoke's settle window
    iq_settle = window("cg_iq_a", 2.7, 3.0)
    if not iq_settle:
        fail("no cg_iq_a samples in the settle window")
    iq_mean = mean(iq_settle)
    if not (40.0 <= iq_mean <= 60.0):
        fail(f"cg_iq_a settle mean {iq_mean:.3f} outside [40,60]")

    # phase 2: OpenGates injection collapses the measured q current
    iq_inj = window("cg_iq_a", 3.4, 3.55)
    if not iq_inj or max(abs(v) for v in iq_inj) >= 2.0:
        fail(f"|cg_iq_a| under OpenGates did not collapse below 2 A "
             f"(max {max(abs(v) for v in iq_inj) if iq_inj else float('nan'):.3f})")

    # and the clear returns the drive to the pre-inject trajectory
    # (the IqVar=150 slew keeps rising during the phase; "recovers" = the
    # post-clear mean is back at/above the pre-inject settled level)
    iq_rec = mean(window("cg_iq_a", 3.75, 3.95))
    if iq_rec < 0.9 * iq_mean:
        fail(f"cg_iq_a post-clear mean {iq_rec:.3f} did not recover "
             f"(pre-inject settle {iq_mean:.3f})")

    # DC link: gen7 bench 144.7 V minus small series drop (pre-inject window;
    # the recovery transient on the slew trajectory is excluded on purpose)
    vdc = window("vdc_v", 0.5, 3.15)
    vdc_mean = mean(vdc)
    if not vdc or not (143.0 <= vdc_mean <= 147.0):
        fail(f"vdc_v mean {vdc_mean:.3f} outside [143,147]")

    # truth-side diagnostics off the graph (NOT the zero stub)
    heat = window("tw_p_heat_w", 1.2, 3.2)
    if not heat or mean(heat) <= 100.0:
        fail(f"tw_p_heat_w load mean {mean(heat):.3f} <= 100 (diag channel dead?)")
    pdc = window("tw_p_dc_w", 1.2, 3.2)
    psh = window("tw_p_shaft_em_w", 1.2, 3.2)
    if not pdc or max(abs(v) for v in pdc) < 200.0:
        fail("tw_p_dc_w missing/zero under load")
    if not psh or max(abs(v) for v in psh) < 200.0:
        fail("tw_p_shaft_em_w missing/zero under load")

    # temperatures finite (non-null) everywhere
    t1 = window("temp_inv1_c", 0.0, 3.95)
    if not t1 or any(not math.isfinite(v) for v in t1):
        fail("temp_inv1_c missing/non-finite")
    if len(t1) < len(frames) * 0.9:
        fail(f"temp_inv1_c sparse: {len(t1)}/{len(frames)}")

    # dclink_p_w present and nonconstant
    dcl = [v for (t, v) in series("dclink_p_w")]
    if len(dcl) < len(frames) * 0.9:
        fail(f"dclink_p_w sparse: {len(dcl)}/{len(frames)}")
    if max(dcl) - min(dcl) < 50.0:
        fail(f"dclink_p_w constant ({min(dcl)}..{max(dcl)})")

    # encoder live inside the healthy learned bounds
    enc = [v for (t, v) in series("EncoderSin")]
    if not enc or not all(11423.0 <= v <= 53864.0 for v in enc):
        bad = [v for v in enc if not (11423.0 <= v <= 53864.0)]
        fail(f"EncoderSin outside healthy bounds: {bad[:3]} (of {len(enc)})")

    print(f"e2e OK: records={s.end_stats.get('final_stats', {}).get('n_records')} "
          f"frames={len(frames)} iq_settle={iq_mean:.2f} iq_rec={iq_rec:.2f} "
          f"vdc={vdc_mean:.2f} heat={mean(heat):.1f} plant={plant}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
