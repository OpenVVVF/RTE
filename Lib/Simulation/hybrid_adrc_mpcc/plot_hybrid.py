#!/usr/bin/env python3
"""Plot hybrid ADRC+MPCC simulation CSVs. Does not touch other result folders."""

from __future__ import annotations

import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd


def plot_case(csv_path: Path, out_dir: Path) -> None:
    df = pd.read_csv(csv_path)
    fig, axes = plt.subplots(3, 1, figsize=(9, 8), sharex=True)

    axes[0].plot(df["time"], df["iq_ref"], "k--", label="iq*")
    axes[0].plot(df["time"], df["iq"], label="iq")
    axes[0].plot(df["time"], df["id"], label="id")
    axes[0].set_ylabel("Current [A]")
    axes[0].legend(loc="best")
    axes[0].grid(True, alpha=0.3)
    axes[0].set_title(csv_path.stem)

    axes[1].plot(df["time"], df["fhat_d"], label="fhat_d")
    axes[1].plot(df["time"], df["fhat_q"], label="fhat_q")
    axes[1].set_ylabel("ESO disturbance")
    axes[1].legend(loc="best")
    axes[1].grid(True, alpha=0.3)

    axes[2].plot(df["time"], df["omega_rpm"], label="rpm")
    axes[2].set_ylabel("Speed [rpm]")
    axes[2].set_xlabel("Time [s]")
    axes[2].grid(True, alpha=0.3)

    fig.tight_layout()
    out = out_dir / "plots" / f"{csv_path.stem}.png"
    out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out, dpi=140)
    plt.close(fig)
    print(f"wrote {out}")


def main() -> int:
    out_dir = Path(sys.argv[1] if len(sys.argv) > 1 else "results/adrc_mpcc_hybrid")
    for name in (
        "case1_iq_step_hybrid.csv",
        "case2_iq_step_L_mismatch_m20_hybrid.csv",
    ):
        p = out_dir / name
        if p.exists():
            plot_case(p, out_dir)
    metrics = out_dir / "metrics_summary.csv"
    if metrics.exists():
        print(metrics.read_text())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
