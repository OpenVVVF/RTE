"""Compare saved current captures and verify estimator behavior; no device I/O."""
from pathlib import Path
import json
import sys
import numpy as np

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT.parent / 'direction_asymmetry'))
from analyze_current_captures import read

def inspect(path):
    a = read(path)
    t = a['time_s']
    theta = np.unwrap(a['theta'])
    fs = 1 / np.median(np.diff(t))
    fe = np.polyfit(t, theta, 1)[0] / (2 * np.pi)
    alternate = 1 - 2 * (a['timer'].astype(np.uint64) >> 31).astype(float)
    fit = np.array([np.ones_like(t), alternate*np.sin(3*theta),
                    alternate*np.cos(3*theta)]).T
    result = dict(file=str(path.relative_to(ROOT.parent.parent)),
                  sample_hz=fs, electrical_hz=fe, rpm=fe*12,
                  bus_mean=float(a['bus'].mean()),
                  seq_step_min=float(np.diff(a['seq']).min()),
                  seq_step_max=float(np.diff(a['seq']).max()),
                  sample_valid_min=float(a['valid'].min()),
                  clipped_fraction=float(np.mean(a['scale'] < .99999)),
                  voltage_scale_min=float(a['scale'].min()))
    for key in ['id', 'iq']:
        raw = a['raw_'+key]
        reconstruction = .25*raw[2:] + .5*raw[1:-1] + .25*raw[:-2]
        b = np.linalg.lstsq(fit, raw, rcond=None)[0]
        residual = raw - fit@b
        result[key] = dict(
            mean=float(a[key].mean()), ac_rms=float(a[key].std()),
            peak_to_peak=float(np.ptp(a[key])), raw_ac_rms=float(raw.std()),
            raw_peak_to_peak=float(np.ptp(raw)),
            estimator_max_error=float(np.max(abs(a[key][2:]-reconstruction))),
            alternating_third_harmonic_amplitude=float(np.hypot(b[1], b[2])),
            alternating_third_variance_fraction=float(1-np.var(residual)/np.var(raw)))
    duty = np.stack([a['active_u'], a['active_v'], a['active_w']])
    # A capture is at twice the PWM frequency. Software-tracked duty estimates
    # imply distance from the zero-vector midpoint to the nearest PWM edge.
    # These are not gate-probe timing measurements.
    half_period_us = 1e6 / fs
    edge_us = np.minimum(duty.min(axis=0), 100-duty.max(axis=0)) * half_period_us/100
    result['estimated_edge_margin_us'] = dict(zip(['min','p10','median'],
                                               map(float,np.percentile(edge_us,[0,10,50]))))
    result['duty_span_percent'] = dict(zip(['min','median','max'],
                                        map(float,np.percentile(np.ptp(duty,axis=0),[0,50,100]))))
    # Reconstruct the speed actually used by feedforward, for the recorded
    # settings Kp=.04, Lambda=.04, Ld=0 and legacy-to-volts scale=.5.
    omega_ff = (a['vqreq']-.02*(a['iqref']-a['iq'])-a['intq'])/.02
    result['feedforward_rpm_median'] = float(np.median(omega_ff)*60/(10*np.pi))
    result['estimator_gain_at_pwm_minus_3fe'] = float(np.sin(np.pi*3*fe/fs)**2)
    result['estimator_gain_at_6fe'] = float(np.cos(np.pi*6*fe/fs)**2)
    return result

paths = [ROOT.parent/'direction_asymmetry/bus75_cycle35_positive_ctrlcap.txt',
         ROOT/'thirtyfive_amp_ctrlcap.txt']
if (ROOT/'bus75_repeat_ctrlcap.txt').exists():
    paths.append(ROOT/'bus75_repeat_ctrlcap.txt')
results = {p.stem: inspect(p) for p in paths}
(ROOT/'investigation.json').write_text(json.dumps(results, indent=2)+'\n')
print(json.dumps(results, indent=2))
