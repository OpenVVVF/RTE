#!/usr/bin/env python3
"""Validate a HostSIL trace CSV.

Checks (hard failures):
  - no NaN/inf in any numeric column
  - i_a + i_b + i_c ~= 0 at every row (zero-sum constraint of the model)
  - |i_x| stays below the software OC limit after the startup transient
Soft expectations (report + fail if grossly violated):
  - omega ramps in the commanded direction and reaches a nontrivial speed
  - duty columns respond to control (not constant, not uniform noise)

usage: validate_trace.py <trace.csv> [--control-start-s T] [--iq-a A]
"""
import argparse
import csv
import math
import sys


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--control-start-s", type=float, default=1.6)
    ap.add_argument("--iq-a", type=float, default=8.0)
    ap.add_argument("--oc-a", type=float, default=200.0,
                    help="software overcurrent bound to check")
    args = ap.parse_args()

    cols = {}
    with open(args.trace) as f:
        r = csv.reader(f)
        header = next(r)
        names = [h.strip() for h in header]
        for h in names:
            cols[h] = []
        for row in r:
            if len(row) != len(names):
                print(f"FAIL: ragged row {row}")
                return 1
            for h, cell in zip(names, row):
                v = float(cell)
                if math.isnan(v) or math.isinf(v):
                    print(f"FAIL: {h} has NaN/inf")
                    return 1
                cols[h].append(v)

    n = len(cols["time_us"])
    if n < 100:
        print(f"FAIL: only {n} rows")
        return 1

    failures = []

    t = [x * 1e-6 for x in cols["time_us"]]

    # Zero-sum currents
    worst_isum = 0.0
    for i in range(n):
        s = cols["i_a"][i] + cols["i_b"][i] + cols["i_c"][i]
        worst_isum = max(worst_isum, abs(s))
    if worst_isum > 1e-3:
        failures.append(f"i_a+i_b+i_c max |sum| {worst_isum:.6g} (want ~0)")

    # Current bounds after startup transient (oc limit, generous)
    imax = 0.0
    for i in range(n):
        if t[i] < args.control_start_s + 0.05:
            continue
        for ph in ("i_a", "i_b", "i_c"):
            imax = max(imax, abs(cols[ph][i]))
    if imax > args.oc_a:
        failures.append(f"|i| max {imax:.3g} A exceeds OC bound {args.oc_a} A")

    # Control region
    ctl = [i for i in range(n) if t[i] >= args.control_start_s + 0.10]
    if not ctl:
        failures.append("no samples after control start")
    else:
        om = cols["omega_e_rad_s"]
        om_end = om[ctl[-1]]
        om_min = min(om[i] for i in ctl)
        if args.iq_a > 0 and (om_end < 10.0 or om_min < -1.0):
            failures.append(
                f"omega did not ramp forward: end={om_end:.4g} min={om_min:.4g}")
        # monotonic-ish: count backward steps beyond ripple
        drops = sum(1 for a, b in zip(ctl, ctl[1:]) if om[b] < om[a] - 0.5)
        if drops > len(ctl) // 10:
            failures.append(f"omega not monotonic-ish: {drops} drops")

        # Duties respond to control: spread well beyond noise.
        for ph in ("duty_u", "duty_v", "duty_w"):
            d = [cols[ph][i] for i in ctl]
            if max(d) - min(d) < 1.0:
                failures.append(f"{ph} static under control "
                                f"(span {max(d) - min(d):.3g} %)")

        # Iq tracking: measured q current should reach a good fraction of ref.
        # Settled window = last 20% of the trace (with sane scenarios that is
        # deep inside the control region). Compare signed: a loop locked onto
        # -Iq for a positive ref must fail, not pass through abs().
        iqm = cols["iq_meas_a"]
        tail0 = int(0.8 * n)
        iq_tail = sum(iqm[tail0:]) / max(1, n - tail0)
        if args.iq_a > 0 and iq_tail < 0.5 * args.iq_a:
            failures.append(
                f"iq_meas mean {iq_tail:.3g} A far below ref {args.iq_a} A")
        elif args.iq_a < 0 and iq_tail > 0.5 * args.iq_a:
            failures.append(
                f"iq_meas mean {iq_tail:.3g} A far above ref {args.iq_a} A")

    imax_all = max(max(map(abs, cols[p])) for p in ("i_a", "i_b", "i_c"))
    print(f"rows={n}  span={t[-1] - t[0]:.3f} s")
    print(f"i_a  [{min(cols['i_a']):.3f}, {max(cols['i_a']):.3f}] A")
    print(f"i_b  [{min(cols['i_b']):.3f}, {max(cols['i_b']):.3f}] A")
    print(f"i_c  [{min(cols['i_c']):.3f}, {max(cols['i_c']):.3f}] A")
    print(f"|i|max={imax_all:.3f} A  isum|max|={worst_isum:.3g} A")
    print(f"duty_u [{min(cols['duty_u']):.2f}, {max(cols['duty_u']):.2f}] %")
    print(f"theta_e [{min(cols['theta_e_rad']):.2f}, "
          f"{max(cols['theta_e_rad']):.2f}] rad")
    print(f"omega_e [{min(cols['omega_e_rad_s']):.2f}, "
          f"{max(cols['omega_e_rad_s']):.2f}] rad/s")
    print(f"rpm_mech end={cols['rpm_mech'][-1]:.1f}")
    print(f"iq_ref  [{min(cols['iq_ref_a']):.3f}, {max(cols['iq_ref_a']):.3f}] A")
    print(f"iq_meas [{min(cols['iq_meas_a']):.3f}, {max(cols['iq_meas_a']):.3f}] A")

    if failures:
        print("VALIDATION FAILED:")
        for f_ in failures:
            print(f"  - {f_}")
        return 1
    print("VALIDATION PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
