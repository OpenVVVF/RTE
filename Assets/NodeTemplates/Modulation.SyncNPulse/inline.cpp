/* Synchronous switching-state modulation, v1: six-step + zero-vector dwell.
 *
 * Above FmSwitchHz electrical, emits direct switching states (0/100 duties)
 * synchronized to the demanded voltage vector angle: the 60-degree sector is
 * selected from atan2(V_Beta, V_Alpha), and amplitude is set by dwelling
 * between the active sector vector and the zero vector using a sigma-delta
 * accumulator on the ISR grid.  Edges are ISR-quantized; scheduling exact
 * edge times via TIM1 compare preload is the follow-up for high speed.
 *
 * Below FmSwitchHz, Active=false and duties sit at 50% so the graph can
 * route async SVPWM duties through the mux instead.
 *
 * N_Pulses currently always reports 1 (six-step) while active; the N-vs-
 * fundamental selection plumbing is FAsyncHz/f_e for future notched
 * patterns (SHE tables). */

const float vdc = V_Dc.in(au::volts);
float poles = Poles;
if (!(poles >= 2.0f)) poles = 10.0f;

const float f_e = fabsf(Omega_Mech) / 60.0f * (poles * 0.5f);
F_Elec = f_e;

const bool active = (f_e >= FmSwitchHz) && (vdc > 1.0f);
Active = active;

if (!active) {
    Duty_A = 50.0f;
    Duty_B = 50.0f;
    Duty_C = 50.0f;
    N_Pulses = 0.0f;
    DwellAcc = 0.0f;
} else {
    const float valpha = V_Alpha.in(au::volts);
    const float vbeta = V_Beta.in(au::volts);

    /* 60-degree sector of the demanded vector; sectors center on the six
     * active vectors (100, 110, 010, 011, 001, 101 at 0,60,...,300 deg). */
    const float ang = atan2f(vbeta, valpha);
    int sector = (int)floorf((ang + 0.52359877559f) * 0.95492965855f);
    sector %= 6;
    if (sector < 0) sector += 6;
    static const int bits[6][3] = {
        {1,0,0},{1,1,0},{0,1,0},{0,1,1},{0,0,1},{1,0,1}
    };

    /* Demanded magnitude relative to the SVPWM linear max (vdc/sqrt(3));
     * the current loop absorbs the six-step Fourier amplitude factor. */
    float m = sqrtf(valpha * valpha + vbeta * vbeta) / (vdc * 0.57735026919f);
    if (m > 1.05f) m = 1.05f;
    if (m < 0.0f) m = 0.0f;

    DwellAcc += m;
    if (DwellAcc >= 1.0f) {
        DwellAcc -= 1.0f;
        Duty_A = bits[sector][0] ? 100.0f : 0.0f;
        Duty_B = bits[sector][1] ? 100.0f : 0.0f;
        Duty_C = bits[sector][2] ? 100.0f : 0.0f;
    } else {
        Duty_A = 0.0f;
        Duty_B = 0.0f;
        Duty_C = 0.0f;
    }
    N_Pulses = 1.0f;
}
