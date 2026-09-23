"""Reproduce the direction-asymmetry summaries from local bench captures."""
from pathlib import Path
import collections
import csv
import json
import os

os.environ.setdefault("MPLCONFIGDIR", "/tmp/rte-direction-mpl")
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parent
observations = json.loads((ROOT / "observations.json").read_text())

def csv_means(path):
    signals = collections.defaultdict(list)
    with path.open() as f:
        for row in csv.DictReader(f):
            signals[row["signal"]].append((float(row["time_s"]), float(row["value"])))
    start = max(v[0][0] for v in signals.values())
    end = min(v[-1][0] for v in signals.values())
    values = {k: np.array([x[1] for x in v if start <= x[0] <= end]) for k, v in signals.items()}
    return start, end, {k: float(v.mean()) for k, v in values.items()}

summary = []
for observation in observations["readings"]:
    start, end, mean = csv_means(ROOT / observation["file"])
    row = dict(observation, duration_s=end-start, rpm=mean["Mech_RPM"],
               id_a=mean["cg_id_a"], iq_a=mean["cg_iq_a"],
               vd_command_v=mean["cg_vd_v"], vq_command_v=mean["cg_vq_v"],
               bus_v=mean["vdc_v"])
    # Model comparison only; commanded voltage is not terminal voltage.
    row["aligned_model_vd_v"] = .0121*row["id_a"] - row["rpm"]*5*2*np.pi/60*.000253*row["iq_a"]
    row["half_command_model_residual_v"] = .5*row["vd_command_v"] - row["aligned_model_vd_v"]
    summary.append(row)
(ROOT / "numeric_summary.json").write_text(json.dumps(summary, indent=2)+"\n")
fields = ["file", "direction", "offset_deg", "regen_w", "duration_s", "rpm", "id_a", "iq_a", "vd_command_v", "vq_command_v", "bus_v", "aligned_model_vd_v", "half_command_model_residual_v"]
with (ROOT / "numeric_summary.csv").open("w", newline="") as f:
    writer = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
    writer.writeheader()
    writer.writerows(summary)

plt.rcParams.update({"font.size": 10, "axes.spines.top": False, "axes.spines.right": False})
colors = {"encoder_positive": "#0072B2", "encoder_negative": "#D55E00"}
fig, axes = plt.subplots(1, 2, figsize=(11, 4), constrained_layout=True)
for direction, label in [("encoder_positive", "+speed / −15 A"), ("encoder_negative", "−speed / +15 A")]:
    points = [r for r in summary if r["direction"] == direction and r.get("note") != "initial A/B point"]
    points.sort(key=lambda r:r["offset_deg"])
    x = [r["offset_deg"] for r in points]
    for ax, key in zip(axes, ["regen_w", "vd_command_v"]):
        ax.plot(x, [r[key] for r in points], "o--", color=colors[direction], label=label)
        ax.set_xlabel("Encoder offset (electrical degrees)")
        ax.set_xticks([120, 135, 150, 165])
        ax.grid(alpha=.2)
axes[0].set(title="Sorensen display: matched ±15 A regen", ylabel="Power returned to supply (W)")
axes[1].set(title="Controller d-voltage command", ylabel="cg_vd_v (V; before PWM scaling)")
axes[0].legend()
fig.suptitle("Dyno-held nominal 500 RPM; manual power readings are approximate")
fig.savefig(ROOT / "offset_sweep.png", dpi=180)
plt.close(fig)

def load_run(label):
    return json.loads((ROOT / ("free_spin_"+label+".json")).read_text())

def crossing_time(rows, speed, descending=False):
    for a, b in zip(rows, rows[1:]):
        va, vb = abs(a["Mech_RPM"]), abs(b["Mech_RPM"])
        crossed = va >= speed >= vb if descending else va <= speed <= vb
        if crossed and va != vb:
            return a["elapsed_s"] + (speed-va)/(vb-va)*(b["elapsed_s"]-a["elapsed_s"])
    return None

runs = {label: load_run(label) for label in ["positive", "negative", "positive_repeat", "positive_25a", "negative_25a"]}
motion = {}
for label, run in runs.items():
    bands = []
    for lo, hi in [(50,150), (150,250), (250,350), (400,500), (500,600)]:
        tlo, thi = [crossing_time(run["rows"], speed) for speed in (lo,hi)]
        coast_rows = [r for r in run.get("coast", []) if r["pwm_moe"] == 0 and r["control_outputs_enabled"] == 0]
        clo, chi = [crossing_time(coast_rows, speed, True) for speed in (lo,hi)]
        band = [r for r in run["rows"] if lo <= abs(r["Mech_RPM"]) < hi]
        bands.append({"lo_rpm":lo, "hi_rpm":hi,
                      "acceleration_rpm_s":(hi-lo)/(thi-tlo) if tlo is not None and thi is not None else None,
                      "coast_deceleration_rpm_s":(hi-lo)/(clo-chi) if clo is not None and chi is not None else None,
                      "poll_samples_in_band":len(band),
                      "reported_mean_iq_a":float(np.mean([r["cg_iq_a"] for r in band])) if band else None})
    motion[label] = {"time_to_600_s":crossing_time(run["rows"], 600), "bands":bands,
                     "maximum_observed_speed_rpm":max(abs(r["Mech_RPM"]) for r in run["rows"]+run.get("coast", []))}
(ROOT / "motion_summary.json").write_text(json.dumps(motion, indent=2)+"\n")
fig, axes = plt.subplots(1,3,figsize=(14,4),constrained_layout=True)
for sign, color in [("positive", "#0072B2"), ("negative", "#D55E00")]:
    for ax, suffix in zip(axes[:2], ["", "_25a"]):
        rows=runs[sign+suffix]["rows"]
        ax.plot([r["elapsed_s"] for r in rows], [abs(r["Mech_RPM"]) for r in rows], color=color, label=sign)
    coast=[r for r in runs[sign+"_25a"]["coast"] if r["pwm_moe"] == 0]
    reference = crossing_time(coast, 500, True)
    coast=[r for r in coast if 300 <= abs(r["Mech_RPM"]) <= 500]
    axes[2].plot([0]+[r["elapsed_s"]-reference for r in coast], [500]+[abs(r["Mech_RPM"]) for r in coast], color=color, label=sign)
for ax, title in zip(axes, ["±15 A acceleration", "±25 A acceleration", "PWM-off coast, aligned at 500 RPM"]):
    ax.set(title=title, xlabel="Elapsed time (s)", ylabel="Absolute encoder speed (RPM)")
    ax.grid(alpha=.2)
axes[0].legend()
axes[2].set_xlabel("Time since crossing 500 RPM (s)")
fig.suptitle("Free-spinning dyno, offset 150°; independent runs from rest")
fig.savefig(ROOT / "free_spin_comparison.png", dpi=180)
plt.close(fig)
print("Wrote numeric_summary.csv/json, motion_summary.json, offset_sweep.png, free_spin_comparison.png")
