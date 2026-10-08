# Gen7 current-ripple timing review

The 75 V / 35 A saved capture has about 1.7 A iq and 3.5 A id AC RMS in
controller feedback. Raw id alternates between PWM extrema; the alternating
third-electrical component explains about 83% of its variance. These recordings
cannot determine how much is winding current versus sensor pickup. The
0.25/0.5/0.25 feedback filter reconstructs correctly, and samples were valid
and consecutive. Switching remains **2.5 kHz**.

Two source defects were corrected offline:

1. The graph sampled an encoder angle early in TIM1 execution and added exactly
   one 200 us update interval. The voltage duties are preloaded and take effect
   at the next timer extremum. The new lead node reads a fresh angle and
   predicts the midpoint of that following interval from TIM1 count/direction.
   This removes the fixed-delay assumption without changing PWM frequency.
   In the saved positive 35 A capture, the commanded angle was about 189 us
   ahead of the current-frame angle; the current callback preceded the capture
   by about 24 us, and the next voltage midpoint followed it by about 210 us.
   Those software timestamps imply roughly 45 us of missing lead at that point.
2. Startup loaded a saved sin/cos ellipse fit before `EncoderADC::init()` cleared
   fit state. The loader now runs after encoder initialization. No fit was active
   in the earlier bench session, so this is a genuine persistence fix, **not**
   an explanation of that session's measured ripple.

The timing change was tested live on 2026-09-24 at 74.3 V, 35 A, and the
unchanged 2.5 kHz switching frequency. Both captures had valid consecutive
samples and no voltage limiting. AC RMS values are from 512 controller frames:

| Rotation | Image | RPM | Iq feedback | Id feedback | Iq raw | Id raw |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Positive | Prior speed image | 2153 | 1.73 A | 3.46 A | 3.54 A | 9.98 A |
| Positive | Timing trial | 2190 | 1.75 A | 3.55 A | 3.62 A | 10.30 A |
| Negative | Prior speed image | -2138 | 1.70 A | 3.33 A | 2.38 A | 9.53 A |
| Negative | Timing trial | -2200 | 1.81 A | 3.66 A | 2.50 A | 10.26 A |

The speeds differ slightly, so the small increases do not establish a regression,
but the trial did not reduce ripple on either axis. The prior speed image was
reflashed after the test (graph hash `bd5390d76c425eb1`); it is idle and
fault-free. The timing trial remains an uncommitted experiment, not a verified
ripple fix. Captures are in `captures/ripple_timing_live_20260924/`.

Increased PI gains already produced an unstable transient, so gains and
inductance feedforward were left untouched. An independent phase-current
measurement is needed to separate physical ripple from ADC/sensor error. The
saved-fit initialization fix is independent of this failed ripple trial.
