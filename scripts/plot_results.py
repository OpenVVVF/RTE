#!/usr/bin/env python3
"""Generate plots and performance metrics from MPC closed-loop CSV logs."""

from __future__ import annotations

import csv
import math
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

RESULTS_DIR = Path(__file__).resolve().parent.parent / "results" / "closed_loop"
PLOTS_DIR = Path(__file__).resolve().parent.parent / "results" / "plots"


def load_csv(path: Path) -> dict[str, np.ndarray]:
    with path.open(newline="") as f:
        reader = csv.DictReader(f)
        rows = list(reader)
    if not rows:
        return {}
    keys = rows[0].keys()
    return {k: np.array([float(r[k]) for r in rows]) for k in keys}


def steady_state_slice(data: dict[str, np.ndarray], frac: float = 0.5) -> slice:
    n = len(data["time"])
    return slice(int(n * frac), n)


def compute_metrics(data: dict[str, np.ndarray]) -> dict[str, float]:
    ss = steady_state_slice(data)
    id_err = data["id"][ss] - data["id_reference"][ss]
    iq_err = data["iq"][ss] - data["iq_reference"][ss]
    torque = data["electromagnetic_torque"][ss]
    ia = data["ia"][ss]
    metrics = {
        "rms_id_error": float(np.sqrt(np.mean(id_err**2))),
        "rms_iq_error": float(np.sqrt(np.mean(iq_err**2))),
        "mean_abs_current_error": float(np.mean(np.abs(np.hstack([id_err, iq_err])))),
        "max_current": float(np.max(np.sqrt(data["id"] ** 2 + data["iq"] ** 2))),
        "torque_ripple_rms": float(np.std(torque)),
        "torque_ripple_pp": float(np.max(torque) - np.min(torque)),
        "mean_mpc_exec_us": float(np.mean(data["controller_execution_time"])),
        "worst_mpc_exec_us": float(np.max(data["controller_execution_time"])),
    }
    # THD on phase-a steady-state current
    x = ia - np.mean(ia)
    n = len(x)
    if n > 16:
        spec = np.fft.rfft(x)
        freqs = np.fft.rfftfreq(n, d=float(np.mean(np.diff(data["time"][ss]))))
        fund_idx = np.argmax(np.abs(spec[1:])) + 1
        fund = np.abs(spec[fund_idx])
        harm = np.sqrt(np.sum(np.abs(spec[fund_idx + 1 :]) ** 2))
        metrics["phase_a_thd_pct"] = float(100.0 * harm / max(fund, 1e-9))
    return metrics


def plot_case(name: str, data: dict[str, np.ndarray]) -> None:
    t = data["time"]
    fig, axes = plt.subplots(4, 2, figsize=(14, 12))
    fig.suptitle(name)

    axes[0, 0].plot(t, data["ia"], label="ia")
    axes[0, 0].plot(t, data["ib"], label="ib")
    axes[0, 0].plot(t, data["ic"], label="ic")
    axes[0, 0].legend(); axes[0, 0].set_ylabel("A")

    axes[0, 1].plot(t, data["id"], label="id")
    axes[0, 1].plot(t, data["id_reference"], "--", label="id*")
    axes[0, 1].plot(t, data["iq"], label="iq")
    axes[0, 1].plot(t, data["iq_reference"], "--", label="iq*")
    axes[0, 1].legend(); axes[0, 1].set_ylabel("A")

    axes[1, 0].plot(t, data["id"] - data["id_reference"])
    axes[1, 0].set_ylabel("id error")
    axes[1, 1].plot(t, data["iq"] - data["iq_reference"])
    axes[1, 1].set_ylabel("iq error")

    axes[2, 0].plot(t, data["mechanical_speed"])
    axes[2, 0].set_ylabel("rad/s")
    axes[2, 1].plot(t, data["electromagnetic_torque"], label="Te")
    axes[2, 1].plot(t, data["load_torque"], label="Tl")
    axes[2, 1].legend(); axes[2, 1].set_ylabel("Nm")

    axes[3, 0].plot(t, data["switching_state"])
    axes[3, 0].set_ylabel("state")
    axes[3, 1].plot(t, data["cost"])
    axes[3, 1].set_ylabel("cost")

    for ax in axes[-1]:
        ax.set_xlabel("time (s)")
    fig.tight_layout()
    PLOTS_DIR.mkdir(parents=True, exist_ok=True)
    fig.savefig(PLOTS_DIR / f"{name}.png", dpi=150)
    plt.close(fig)


def main() -> None:
    summary_lines = ["case,metric,value"]
    for csv_path in sorted(RESULTS_DIR.glob("*.csv")):
        data = load_csv(csv_path)
        if not data:
            continue
        plot_case(csv_path.stem, data)
        metrics = compute_metrics(data)
        for k, v in metrics.items():
            summary_lines.append(f"{csv_path.stem},{k},{v:.6g}")
    out = Path(__file__).resolve().parent.parent / "results" / "metrics_summary.csv"
    out.write_text("\n".join(summary_lines) + "\n")
    print(f"Wrote plots to {PLOTS_DIR} and metrics to {out}")


if __name__ == "__main__":
    main()
