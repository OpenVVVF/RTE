"""Analyze saved baseline captures; no hardware access."""
from pathlib import Path
import json
import sys
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT.parent / 'direction_asymmetry'))
from analyze_current_captures import read, stats

names = ['zero', 'ten_amp', 'thirtyfive_amp']
summary = {}
fig, axes = plt.subplots(3, 1, figsize=(11, 9), constrained_layout=True)
for ax, name in zip(axes, names):
    data = read(ROOT / (name + '_ctrlcap.txt'))
    result = stats(data)
    for key in ['id', 'iq', 'raw_id', 'raw_iq']:
        result[key]['peak_to_peak'] = result[key]['max'] - result[key]['min']
        # This comparison reports time-domain ripple, not harmonic fits.
        for field in list(result[key]):
            if field.startswith('third_'):
                del result[key][field]
    for key in ['bus', 'scale', 'age_us', 'iqref']:
        result[key] = dict(min=float(data[key].min()), max=float(data[key].max()),
                           mean=float(data[key].mean()))
    result['clipped_fraction'] = float(np.mean(data['scale'] < 0.99999))
    summary[name] = result
    t = data['time_s'] * 1000
    ax.plot(t, data['raw_iq'], color='#95a3b3', lw=0.8, label='Raw iq')
    ax.plot(t, data['iq'], color='#1261a0', lw=1.1, label='Controller feedback iq')
    ax.plot(t, data['iqref'], color='#d06a15', lw=1, ls='--', label='Iq request')
    ax.set_title(f"{data['iqref'].mean():.0f} A request, {result['encoder_angle_fit_rpm']:.0f} RPM | "
                 f"feedback {result['iq']['ac_rms']:.2f} A AC RMS / {result['iq']['peak_to_peak']:.2f} A peak-to-peak")
    ax.set_ylabel('Current (A)')
    ax.set_xlabel('Capture time (ms)')
    ax.grid(alpha=0.2)
axes[0].legend(loc='upper right', ncol=3, fontsize=8)
fig.suptitle('5b529e2 baseline: 512 samples per capture, approximately 5 kHz\n'
             'About 49 V bus; voltage limit 0.45 at zero current, 0.54 for spin tests')
fig.savefig(ROOT / 'iq_comparison.png', dpi=160)
(ROOT / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
for name, result in summary.items():
    print(name, 'rpm', round(result['encoder_angle_fit_rpm'], 1),
          'iq mean', round(result['iq']['mean'], 3),
          'iq AC RMS', round(result['iq']['ac_rms'], 3),
          'iq peak-to-peak', round(result['iq']['peak_to_peak'], 3),
          'raw iq peak-to-peak', round(result['raw_iq']['peak_to_peak'], 3))
