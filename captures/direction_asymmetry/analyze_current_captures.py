"""Analyze bounded ctrlcap exports; no device access or motor commands."""
from pathlib import Path
import json
import numpy as np

ROOT = Path(__file__).resolve().parent

def read(path):
    records = {}
    headers = {}
    for line in path.read_text().splitlines():
        for kind in ("ccA", "ccB"):
            marker = kind + ":"
            if marker not in line:
                continue
            fields = line.split(marker, 1)[1].split(",")
            if fields[0] == "index":
                headers[kind] = fields
            else:
                values = dict(zip(headers[kind], map(float, fields)))
                records.setdefault(int(values["index"]), {}).update(values)
    rows = [records[k] for k in sorted(records)]
    assert len(rows) == 512, (path, len(rows))
    # A 5 kHz controller with legacy 2.5 kHz sampling repeated each frame.
    rows = [r for i, r in enumerate(rows) if not i or r["seq"] != rows[i-1]["seq"]]
    arrays = {k: np.array([r[k] for r in rows]) for k in rows[0]}
    cycles = arrays["cycles"]
    arrays["time_s"] = np.r_[0, np.cumsum(np.mod(np.diff(cycles), 2**32))] / 550e6
    return arrays

def stats(a):
    t = a["time_s"]
    theta = np.unwrap(a["theta"])
    speed = np.polyfit(t, theta, 1)[0] * 60 / (10*np.pi)
    result = dict(samples=len(t), sample_hz=float(1/np.median(np.diff(t))),
                  encoder_angle_fit_rpm=float(speed), valid_min=float(a["valid"].min()),
                  voltage_scale_min=float(a["scale"].min()))
    design = np.array([np.ones_like(t), t, *[f(k*theta) for k in range(1,7)
                                         for f in (np.sin,np.cos)]]).T
    for key in ("id", "iq", "raw_id", "raw_iq"):
        if key not in a:
            continue
        y = a[key]
        item = dict(min=float(y.min()), max=float(y.max()), mean=float(y.mean()),
                    ac_rms=float(y.std()))
        # Only interpret single-phase-of-PWM harmonic fits. Interleaved
        # opposite midpoint errors move to sidebands near half the sample rate.
        if abs(speed) > 100:
            c = np.linalg.lstsq(design,y,rcond=None)[0]
            item["third_electrical_sin_cos"] = c[6:8].tolist()
            item["third_electrical_amplitude"] = float(np.hypot(c[6],c[7]))
        result[key] = item
    return result

if __name__ == "__main__":
    summary = {p.stem: stats(read(p)) for p in sorted(ROOT.glob("bus75_*_ctrlcap.txt"))}
    (ROOT / "bus75_current_summary.json").write_text(json.dumps(summary,indent=2)+"\n")
    for name, result in summary.items():
        print(name, "rpm=%.1f" % result["encoder_angle_fit_rpm"],
              "id AC RMS=%.3f" % result["id"]["ac_rms"],
              "iq mean=%.3f AC RMS=%.3f" % (result["iq"]["mean"],result["iq"]["ac_rms"]))
