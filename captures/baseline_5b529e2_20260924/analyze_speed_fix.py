"""Compare saved speed-fix/gain-trial captures. No device access."""
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

settings = {
    'bus75_repeat': (.04, .04),
    'speed_fix': (.04, .04),
    'kp008_low': (.08, .08),
    'kp008': (.08, .08),
    'kpd002': (.02, .04),
    'speed_fix_negative': (.04, .04),
}
results = {}
for name, (kpd, kpq) in settings.items():
    a = read(ROOT / (name + '_ctrlcap.txt'))
    result = stats(a)
    result['kpd'], result['kpq'] = kpd, kpq
    result['bus_mean'] = float(a['bus'].mean())
    result['clipped_fraction'] = float(np.mean(a['scale'] < .99999))
    result['sequence_increment_min'] = float(np.diff(a['seq']).min())
    result['sequence_increment_max'] = float(np.diff(a['seq']).max())
    # All trials retained Lambda=.04, Ld/Lq=0, Knee=0 and legacy scale=.5.
    rpm_ff = (a['vqreq'] - .5*kpq*(a['iqref']-a['iq']) - a['intq']) / .02 * 60/(10*np.pi)
    result['feedforward_rpm_mean'] = float(rpm_ff.mean())
    result['speed_lag_rpm'] = result['encoder_angle_fit_rpm'] - float(rpm_ff.mean())
    for axis in ['iq', 'id']:
        result[axis]['peak_to_peak'] = float(np.ptp(a[axis]))
        result['raw_' + axis]['peak_to_peak'] = float(np.ptp(a['raw_' + axis]))
        reconstruction = .25*a['raw_'+axis][2:] + .5*a['raw_'+axis][1:-1] + .25*a['raw_'+axis][:-2]
        result[axis]['estimator_max_error'] = float(np.max(abs(a[axis][2:] - reconstruction)))
    results[name] = result
(ROOT / 'speed_fix_summary.json').write_text(json.dumps(results, indent=2) + '\n')

fig, axes = plt.subplots(2, 2, figsize=(11, 6), sharex=True, constrained_layout=True)
for col, name in enumerate(['bus75_repeat', 'speed_fix']):
    a = read(ROOT / (name + '_ctrlcap.txt'))
    s = results[name]
    for row, axis in enumerate(['iq', 'id']):
        ax = axes[row, col]
        ax.plot(a['time_s']*1000, a['raw_'+axis], color='#bbc4ca', lw=.7, label='Raw ADC d/q')
        ax.plot(a['time_s']*1000, a[axis], color='#1261a0', lw=1, label='Controller feedback')
        ax.axhline(a[axis+'ref'].mean(), color='#a35514', ls='--', lw=1, label='Request')
        ax.set_title(f"{axis}: {s[axis]['ac_rms']:.2f} A AC RMS, {s[axis]['peak_to_peak']:.2f} A p-p")
        ax.set_ylim((20, 50) if axis == 'iq' else (-25, 25))
        ax.set_ylabel('Current (A)')
        ax.grid(alpha=.2)
        if row == 1: ax.set_xlabel('Capture time (ms)')
axes[0, 0].legend(fontsize=8)
fig.suptitle('75 V bus, 35 A iq / 0 A id, 2.5 kHz switching unchanged\n'
             'Left: baseline at 2,078 RPM; right: speed fix at 2,153 RPM (separate acceleration captures)')
fig.savefig(ROOT / 'speed_fix_comparison.png', dpi=160)
for name, s in results.items():
    print(f"{name}: {s['encoder_angle_fit_rpm']:.1f} RPM, speed lag {s['speed_lag_rpm']:.1f} RPM; "
          f"iq {s['iq']['ac_rms']:.3f} A, id {s['id']['ac_rms']:.3f} A AC RMS")
