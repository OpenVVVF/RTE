#!/usr/bin/env python3
"""Plot FOC vs MPC paper comparison results (Gen6 bench study)."""

from __future__ import annotations

import csv
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / "results" / "paper_foc_vs_mpc"
PLOT = DATA / "plots"


def load(path: Path) -> dict[str, np.ndarray]:
    with path.open(newline="") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        return {}
    out: dict[str, np.ndarray] = {}
    for k in rows[0]:
        if k == "controller":
            out[k] = np.array([r[k] for r in rows])
        else:
            out[k] = np.array([float(r[k]) for r in rows])
    return out


def main() -> None:
    foc1 = load(DATA / "case1_iq_step_foc.csv")
    mpc1 = load(DATA / "case1_iq_step_mpc.csv")
    foc2 = load(DATA / "case2_speed_2000_foc.csv")
    mpc2 = load(DATA / "case2_speed_2000_mpc.csv")
    if not foc1 or not mpc1 or not foc2 or not mpc2:
        raise SystemExit(
            "Missing CSVs. Run:\n"
            "  ./build/Lib/Simulation/paper_foc_vs_mpc\n"
            "from the MPC_Three-phase_PMSM directory first."
        )

    PLOT.mkdir(parents=True, exist_ok=True)

    # Case 1
    fig, axes = plt.subplots(2, 2, figsize=(12, 8))
    fig.suptitle("Case 1 — Current loop: FOC vs FCS-MPCC (Optimal Duty)\niq* = 10 A, Gen6 bench params, Ts = 200 µs")

    t = foc1["time"]
    axes[0, 0].plot(t, foc1["iq"], "--", label="FOC iq", lw=1.8)
    axes[0, 0].plot(t, mpc1["iq"], label="MPC iq", lw=1.5)
    axes[0, 0].plot(t, foc1["iq_ref"], "k:", label="iq*")
    axes[0, 0].set_ylabel("iq [A]")
    axes[0, 0].legend(fontsize=8)
    axes[0, 0].grid(True, alpha=0.3)

    axes[0, 1].plot(t, foc1["id"], "--", label="FOC id")
    axes[0, 1].plot(t, mpc1["id"], label="MPC id")
    axes[0, 1].axhline(0.0, color="k", ls=":", lw=1)
    axes[0, 1].set_ylabel("id [A]")
    axes[0, 1].legend(fontsize=8)
    axes[0, 1].grid(True, alpha=0.3)

    axes[1, 0].plot(t, foc1["te"], "--", label="FOC Te")
    axes[1, 0].plot(t, mpc1["te"], label="MPC Te")
    axes[1, 0].set_ylabel("Te [Nm]")
    axes[1, 0].set_xlabel("Time [s]")
    axes[1, 0].legend(fontsize=8)
    axes[1, 0].grid(True, alpha=0.3)

    axes[1, 1].plot(t, foc1["ia"], "--", label="FOC ia", alpha=0.85)
    axes[1, 1].plot(t, mpc1["ia"], label="MPC ia", alpha=0.85)
    axes[1, 1].set_ylabel("ia [A]")
    axes[1, 1].set_xlabel("Time [s]")
    axes[1, 1].legend(fontsize=8)
    axes[1, 1].grid(True, alpha=0.3)

    fig.tight_layout()
    p1 = PLOT / "case1_iq_step_foc_vs_mpc.png"
    fig.savefig(p1, dpi=150)
    print(f"Wrote {p1}")

    # Case 2
    fig2, axes2 = plt.subplots(2, 2, figsize=(12, 8))
    fig2.suptitle(
        "Case 2 — Speed regulation to 2000 rpm (same outer PI)\n"
        "Load step 0→1 Nm at t=0.25 s — FOC (PI+SVPWM) vs MPC (deadbeat+SVPWM)"
    )
    t2 = foc2["time"]
    axes2[0, 0].plot(t2, foc2["omega_rpm"], "--", label="FOC", lw=1.8)
    axes2[0, 0].plot(t2, mpc2["omega_rpm"], label="MPC", lw=1.5)
    axes2[0, 0].axhline(2000.0, color="k", ls=":", label="2000 rpm")
    axes2[0, 0].axvline(0.25, color="0.5", ls="--", alpha=0.6)
    axes2[0, 0].set_ylabel("Speed [rpm]")
    axes2[0, 0].legend(fontsize=8)
    axes2[0, 0].grid(True, alpha=0.3)

    axes2[0, 1].plot(t2, foc2["iq"], "--", label="FOC iq")
    axes2[0, 1].plot(t2, mpc2["iq"], label="MPC iq")
    axes2[0, 1].plot(t2, foc2["iq_ref"], ":", color="C0", alpha=0.5, label="FOC iq*")
    axes2[0, 1].plot(t2, mpc2["iq_ref"], ":", color="C1", alpha=0.5, label="MPC iq*")
    axes2[0, 1].set_ylabel("iq [A]")
    axes2[0, 1].legend(fontsize=8)
    axes2[0, 1].grid(True, alpha=0.3)

    axes2[1, 0].plot(t2, foc2["te"], "--", label="FOC Te")
    axes2[1, 0].plot(t2, mpc2["te"], label="MPC Te")
    axes2[1, 0].plot(t2, foc2["tl"], "k:", label="Tl")
    axes2[1, 0].set_ylabel("Torque [Nm]")
    axes2[1, 0].set_xlabel("Time [s]")
    axes2[1, 0].legend(fontsize=8)
    axes2[1, 0].grid(True, alpha=0.3)

    axes2[1, 1].plot(t2, foc2["id"], "--", label="FOC id")
    axes2[1, 1].plot(t2, mpc2["id"], label="MPC id")
    axes2[1, 1].set_ylabel("id [A]")
    axes2[1, 1].set_xlabel("Time [s]")
    axes2[1, 1].legend(fontsize=8)
    axes2[1, 1].grid(True, alpha=0.3)

    fig2.tight_layout()
    p2 = PLOT / "case2_speed_2000_foc_vs_mpc.png"
    fig2.savefig(p2, dpi=150)
    print(f"Wrote {p2}")

    # Print metrics table if present
    metrics = DATA / "metrics_summary.csv"
    if metrics.exists():
        print("\nMetrics summary:")
        print(metrics.read_text())


if __name__ == "__main__":
    main()
