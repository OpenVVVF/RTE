#!/usr/bin/env python3
"""SSO-latency fault-injection run harness (OpenVVVF test 20, OV-TEST-HW-SSO-LATENCY).

One invocation = one injection run:
  1. (optional) re-establish operating point: fault clear, control start, set IqVar
     (IqVar resets to 0 on every control start, so it is always set after).
  2. Settle, snapshot pre-injection state.
  3. Host-timestamped injection command; capture device console response.
  4. Post-injection snapshot; export telemetry CSV + fault string history + console tail.
  5. Analyze the CSV: bound detection-to-SSO latency in the device timebase.
"""
import argparse
import json
import os
import subprocess
import sys
import time

RTE = "/home/tliao/Desktop/RTE/build/bin/rte"
OUTDIR = "/home/tliao/Desktop/RTE/captures/sso_latency_20260925"
SIGNALS = ["cg_iq_a", "cg_id_a", "cg_iu_a", "cg_iv_a", "cg_iw_a",
           "pwm_moe", "gate_ready", "control_outputs_enabled"]


def rte_tool(tool, args):
    r = subprocess.run([RTE, "tool", tool, "--arguments", json.dumps(args)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"{tool} failed: {r.stderr.strip() or r.stdout[:500]}")
    return json.loads(r.stdout)


def cmd(command, lines=10):
    d = rte_tool("rte_device_command_response",
                 {"command": command, "max_lines": lines, "timeout_ms": 2500})
    return [l["text"] for l in d.get("observed_lines", [])]


def snapshot(signals):
    d = rte_tool("rte_device_snapshot", {"signals": signals})
    return {k: v.get("value") for k, v in d.get("values", {}).items()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--run", required=True, help="run id, e.g. SSO-LAT_run2_fwsso")
    ap.add_argument("--inject", required=True, help="device command that injects the fault")
    ap.add_argument("--iq", type=float, default=-15.0)
    ap.add_argument("--settle", type=float, default=15.0)
    ap.add_argument("--restart", action="store_true",
                    help="fault clear + control start + set IqVar before settling")
    ap.add_argument("--pre", action="append", default=[],
                    help="device command to run after fault clear, before control start")
    ap.add_argument("--no-iq", action="store_true",
                    help="do not set IqVar on restart (injection command sets it)")
    args = ap.parse_args()

    os.makedirs(OUTDIR, exist_ok=True)
    rec = {"run": args.run, "inject": args.inject, "iq": args.iq, "steps": []}

    if args.restart:
        rec["steps"].append({"fault_clear": cmd("fault clear", 15)})
        for pre in args.pre:
            rec["steps"].append({f"pre: {pre}": cmd(pre)})
        rec["steps"].append({"control_start": cmd("control start")})
        if not args.no_iq:
            rec["steps"].append({"set_iq": cmd(f"var set IqVar {args.iq}")})

    print(f"[{args.run}] settling {args.settle:.0f}s at IqVar={args.iq} ...", flush=True)
    time.sleep(args.settle)

    pre = snapshot(["cg_iq_a", "Mech_RPM", "pwm_moe", "gate_ready",
                    "control_state", "vdc_v", "control_outputs_enabled"])
    rec["pre_injection"] = pre
    print(f"[{args.run}] pre: {pre}", flush=True)

    t0 = time.time()
    resp = cmd(args.inject, 15)
    t1 = time.time()
    rec["injection"] = {"host_t0": t0, "host_t1": t1, "console": resp}
    print(f"[{args.run}] injected at host {t0:.3f}: {resp}", flush=True)

    time.sleep(2.0)
    post = snapshot(["cg_iq_a", "Mech_RPM", "pwm_moe", "gate_ready",
                     "control_state", "fault_flags_hex", "control_outputs_enabled"])
    rec["post_injection"] = post
    print(f"[{args.run}] post: {post}", flush=True)

    csv_path = os.path.join(OUTDIR, f"{args.run}_2026-09-25.csv")
    rte_tool("rte_device_history_export",
             {"signals": SIGNALS, "output": csv_path, "limit": 12000})
    rec["csv"] = csv_path

    faults = rte_tool("rte_device_string_history",
                      {"signal": "fault_active_names", "limit": 20})
    rec["fault_string_history"] = faults

    console = rte_tool("rte_device_console", {"lines": 25})
    rec["console_tail"] = console

    rec_path = os.path.join(OUTDIR, f"{args.run}_2026-09-25.json")
    with open(rec_path, "w") as f:
        json.dump(rec, f, indent=1)

    # --- latency analysis in device timebase ---
    import csv as csvmod
    series = {}
    with open(csv_path) as f:
        rdr = csvmod.DictReader(f)
        cols = rdr.fieldnames
        for row in rdr:
            sig = row.get("signal") or row.get("name")
            t = float(row["time_s"])
            v = float(row["value"])
            series.setdefault(sig, []).append((t, v))

    def bound(sig, pred_normal, pred_fault):
        pts = series.get(sig, [])
        first_fault = None
        for t, v in pts:
            if pred_fault(v):
                first_fault = t
                break
        last_normal = None
        for t, v in pts:
            if t < (first_fault or 1e18) and pred_normal(v):
                last_normal = t
        return last_normal, first_fault

    moe_lo, moe_hi = bound("pwm_moe", lambda v: v > 0.5, lambda v: v < 0.5)
    iq_pts = series.get("cg_iq_a", [])
    if iq_pts and moe_lo is not None:
        iq_pre = [v for t, v in iq_pts if moe_lo - 1.0 <= t <= moe_lo]
        iq_base = sum(iq_pre) / len(iq_pre) if iq_pre else 0.0
    elif iq_pts:
        iq_base = sum(v for _, v in iq_pts[-50:]) / len(iq_pts[-50:])
    else:
        iq_base = 0.0
    thr = max(abs(iq_base) * 0.5, 1.0)
    iq_lo, iq_hi = bound("cg_iq_a", lambda v: abs(v) > thr, lambda v: abs(v) <= thr)
    print(f"[{args.run}] iq baseline {iq_base:.2f} A, collapse threshold {thr:.2f} A")
    print(f"[{args.run}] pwm_moe 1->0: last normal t={moe_lo}, first faulted t={moe_hi}")
    print(f"[{args.run}] iq collapse:     last normal t={iq_lo}, first faulted t={iq_hi}")
    if moe_lo and moe_hi:
        print(f"[{args.run}] SSO latency bound (MOE): <= {1000*(moe_hi-moe_lo):.1f} ms (telemetry resolution)")
    if iq_lo and iq_hi:
        print(f"[{args.run}] SSO latency bound (iq):  <= {1000*(iq_hi-iq_lo):.1f} ms (telemetry resolution)")
    print(f"[{args.run}] evidence: {rec_path} | {csv_path}")


if __name__ == "__main__":
    main()
