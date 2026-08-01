/* Synchronous switching-state modulation, v2: PWM'd six-step (120 deg
 * trapezoid), amplitude taken from the commanded voltage vector.
 *
 * Above FmSwitchHz electrical, each phase keeps switching complementarily at
 * the carrier while the AVERAGE phase voltage follows a six-step trapezoid
 * synchronized to the demanded vector angle (from the current loop).  The
 * commanded magnitude m scales the trapezoid between 0 and the DC rail, so
 * phase current stays PI-regulated exactly as under SVPWM - no full-rail
 * dwell states, safe for milliohm motors.  Commutation notches are
 * angle-referenced (synchronous), not carrier-referenced.
 *
 * Below FmSwitchHz, Active=false and duties sit at 50% so the graph can
 * route async SVPWM duties through the mux instead.
 *
 * N_Pulses currently reports 1 (six-step / one pulse per half cycle).  True
 * n-pulse SHE notch tables extend the same structure: pattern(deg, m, N)
 * replacing the trapezoid.  Edges are ISR-quantized; scheduling exact edge
 * times via TIM1 compare preload is the follow-up for high speed. */

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
    const float ang = atan2f(vbeta, valpha);

    /* Commanded magnitude relative to the SVPWM linear max (vdc/sqrt(3));
     * trapezoid top plateaus reach the rail at m = 1. */
    float m = sqrtf(valpha * valpha + vbeta * vbeta) / (vdc * 0.57735026919f);
    if (m > 1.0f) m = 1.0f;
    if (m < 0.0f) m = 0.0f;

    /* 120-degree trapezoidal references in [-1, 1]: +1 plateau for 120 deg,
     * linear transition over the 60-degree sector boundaries. */
    auto trap = [](float x) -> float {
        const float two_pi = 6.28318530718f;
        x = fmodf(x, two_pi);
        if (x < 0.0f) x += two_pi;
        const float deg60 = 1.04719755120f;
        if (x < deg60) return 1.0f;
        if (x < 2.0f * deg60) return 1.0f - 2.0f * (x - deg60) / deg60;
        if (x < 4.0f * deg60) return -1.0f;
        if (x < 5.0f * deg60) return -1.0f + 2.0f * (x - 4.0f * deg60) / deg60;
        return 1.0f;
    };

    const float ta = trap(ang);
    const float tb = trap(ang - 2.09439510239f);
    const float tc = trap(ang + 2.09439510239f);

    Duty_A = 50.0f + 50.0f * m * ta;
    Duty_B = 50.0f + 50.0f * m * tb;
    Duty_C = 50.0f + 50.0f * m * tc;
    N_Pulses = 1.0f;
}
