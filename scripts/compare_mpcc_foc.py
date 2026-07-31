#!/usr/bin/env python3
"""Overlay MPCC (conventional/improved) vs FOC plots on Mac."""

from __future__ import annotations

import csv
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

ROOT = Path(__file__).resolve().parent.parent
DATA_DIR = ROOT / "results" / "comparison"
PLOT_DIR = ROOT / "results" / "comparison" / "plots"


def load_csv(path: Path) -> dict[str, np.ndarray]:
    with path.open(newline="") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        return {}
    out: dict[str, np.ndarray] = {}
    for k in rows[0]:
        if k == "controller":
            out[k] = np.array([row[k] for row in rows])
        else:
            out[k] = np.array([float(row[k]) for row in rows])
    return out


def main() -> None:
    mpcc = load_csv(DATA_DIR / "mpcc.csv")
    mpcc_opt = load_csv(DATA_DIR / "mpcc_opt.csv")
    foc = load_csv(DATA_DIR / "foc.csv")
    if not mpcc or not mpcc_opt or not foc:
        raise SystemExit(
            "Missing CSV files. Run:\n"
            "  ./build/Lib/Simulation/compare_mpcc_foc\n"
            "first."
        )

    PLOT_DIR.mkdir(parents=True, exist_ok=True)
    t = mpcc["time"]

    fig, axes = plt.subplots(2, 2, figsize=(13, 9))
    fig.suptitle("FOC vs MPCC (Conventional/Optimal Duty) — load step 0→5 Nm at t=0.03 s, iq*=10 A")

    axes[0, 0].plot(t, foc["ia"], "--", label="FOC ia")
    axes[0, 0].plot(t, mpcc["ia"], label="MPCC-conv ia")
    axes[0, 0].plot(t, mpcc_opt["ia"], label="MPCC-opt ia", alpha=0.8)
    axes[0, 0].set_ylabel("Phase current [A]")
    axes[0, 0].legend(fontsize=8)
    axes[0, 0].grid(True, alpha=0.3)

    axes[0, 1].plot(t, foc["id"], "--", label="FOC id")
    axes[0, 1].plot(t, mpcc["id"], label="MPCC-conv id")
    axes[0, 1].plot(t, mpcc_opt["id"], label="MPCC-opt id", alpha=0.8)
    axes[0, 1].plot(t, foc["iq"], "--", label="FOC iq")
    axes[0, 1].plot(t, mpcc["iq"], label="MPCC-conv iq")
    axes[0, 1].plot(t, mpcc_opt["iq"], label="MPCC-opt iq", alpha=0.8)
    axes[0, 1].plot(t, mpcc["id_reference"], "k:", label="id*")
    axes[0, 1].plot(t, mpcc["iq_reference"], "k-.", label="iq*")
    axes[0, 1].set_ylabel("dq current [A]")
    axes[0, 1].legend(fontsize=8)
    axes[0, 1].grid(True, alpha=0.3)

    axes[1, 0].plot(t, foc["electromagnetic_torque"], "--", label="FOC Te")
    axes[1, 0].plot(t, mpcc["electromagnetic_torque"], label="MPCC-conv Te")
    axes[1, 0].plot(t, mpcc_opt["electromagnetic_torque"], label="MPCC-opt Te", alpha=0.8)
    axes[1, 0].plot(t, mpcc["load_torque"], "k:", label="Load Tl")
    axes[1, 0].set_ylabel("Torque [Nm]")
    axes[1, 0].set_xlabel("Time [s]")
    axes[1, 0].legend(fontsize=8)
    axes[1, 0].grid(True, alpha=0.3)

    axes[1, 1].plot(t, foc["mechanical_speed"], "--", label="FOC speed")
    axes[1, 1].plot(t, mpcc["mechanical_speed"], label="MPCC-conv speed")
    axes[1, 1].plot(t, mpcc_opt["mechanical_speed"], label="MPCC-opt speed", alpha=0.8)
    axes[1, 1].set_ylabel("Mechanical speed [rad/s]")
    axes[1, 1].set_xlabel("Time [s]")
    axes[1, 1].legend(fontsize=8)
    axes[1, 1].grid(True, alpha=0.3)

    fig.tight_layout()
    out = PLOT_DIR / "mpcc_vs_foc_opt.png"
    fig.savefig(out, dpi=150)
    print(f"Saved comparison figure: {out}")
    if plt.get_backend().lower() != "agg":
        plt.show()


if __name__ == "__main__":
    main()
