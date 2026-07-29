#!/usr/bin/env python3
"""Plot HostSim CSV traces for offline inspection."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

import matplotlib.pyplot as plt


def load_trace(path: Path) -> dict[str, list[float]]:
    cols: dict[str, list[float]] = {}
    with path.open(newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            for k, v in row.items():
                cols.setdefault(k, []).append(float(v))
    return cols


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", type=Path, help="trace.csv from HostSim")
    parser.add_argument("-o", "--output", type=Path, help="save figure instead of showing")
    args = parser.parse_args()

    data = load_trace(args.csv)
    t_ms = [x / 1000.0 for x in data["time_us"]]

    fig, axes = plt.subplots(4, 1, figsize=(10, 9), sharex=True)
    fig.suptitle(f"HostSim trace: {args.csv.name}")

    axes[0].plot(t_ms, data["throttle_a"], label="throttle_a")
    axes[0].plot(t_ms, data["throttle_b"], label="throttle_b", alpha=0.7)
    axes[0].set_ylabel("throttle")
    axes[0].legend(loc="upper right")
    axes[0].grid(True, alpha=0.3)

    axes[1].plot(t_ms, data["duty_u"], label="duty_u")
    axes[1].plot(t_ms, data["duty_v"], label="duty_v")
    axes[1].plot(t_ms, data["duty_w"], label="duty_w")
    axes[1].set_ylabel("duty %")
    axes[1].legend(loc="upper right", ncol=3, fontsize=8)
    axes[1].grid(True, alpha=0.3)

    axes[2].plot(t_ms, data["i_a"], label="i_a")
    axes[2].plot(t_ms, data["i_b"], label="i_b")
    axes[2].plot(t_ms, data["i_c"], label="i_c")
    axes[2].set_ylabel("current [A]")
    axes[2].legend(loc="upper right", ncol=3, fontsize=8)
    axes[2].grid(True, alpha=0.3)

    axes[3].plot(t_ms, data["theta_e"], label="theta_e [deg]")
    ax3b = axes[3].twinx()
    ax3b.plot(t_ms, data["omega_e"], color="tab:red", alpha=0.6, label="omega_e")
    axes[3].set_ylabel("angle [deg]")
    ax3b.set_ylabel("omega_e [rad/s]")
    axes[3].set_xlabel("time [ms]")
    axes[3].grid(True, alpha=0.3)

    fig.tight_layout()
    if args.output:
        fig.savefig(args.output, dpi=150)
        print(f"wrote {args.output}")
    else:
        plt.show()


if __name__ == "__main__":
    main()
