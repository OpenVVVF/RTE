/* Synchronous n-pulse PWM (synchronized sampled sine) - the ladder's
 * intermediate sync mode.
 *
 * The pattern is locked to the rotor angle: duties follow a cosine
 * referenced to Theta_E + delta, slot-averaged over 2*pi/N slots, with N
 * chosen from the chopping-rate budget.  The timer keeps chopping at the
 * async carrier, so current ripple stays carrier-bounded at any speed.
 *
 * Stability (bench-learned):
 * - The commanded phase delta = atan2(V_Q, V_D) and magnitude m are
 *   SLEW-LIMITED.  On hard decel the PI command jumps tens of degrees in
 *   one step; at high back-EMF a slot-quantized phase jump is volts into a
 *   low-L motor and self-oscillates.  TrackDelta/TrackM follow the command
 *   with bounded rate; Theta_E still enters unfiltered (synchronism kept).
 * - On activation (gate rising edge) the tracker ARMs to the current
 *   command, so the SVPWM -> sync handoff is phase/magnitude continuous.
 * - N changes require the new value to persist (NChangeT > NHoldMs) to stop
 *   pulse-count chatter re-slotting the pattern during transients.
 *
 * Phase references are cos (SVPWM's phase A follows valpha = |V|*cos(phi);
 * using sin rotated the vector 90 deg into the d-axis and blew up the
 * bench).
 *
 * Contract: ModeId-gated, holds 50% neutral otherwise; vdc < 1 V -> 50%. */

const float vdc = V_Dc.in(au::volts);
const bool running = (ModeId == ModeFrom) || (ModeId == ModeTo);

if (!running || !(vdc > 1.0f) || !(F_Elec > 0.01f) || !(Dt > 0.0f)) {
    Duty_A = 50.0f;
    Duty_B = 50.0f;
    Duty_C = 50.0f;
    Armed = 0.0f;
} else {
    const float vd = V_D.in(au::volts);
    const float vq = V_Q.in(au::volts);

    /* Commanded targets. */
    const float delta_t = atan2f(vq, vd);
    float m_t = sqrtf(vd * vd + vq * vq) / (vdc * 0.57735026919f);
    if (m_t > 1.0f) m_t = 1.0f;
    if (m_t < 0.0f) m_t = 0.0f;

    /* Arm on activation: start exactly on the commanded vector. */
    if (Armed < 0.5f) {
        Armed = 1.0f;
        TrackDelta = delta_t;
        TrackM = m_t;
        NChangeT = 0.0f;
    }

    /* Slew-limit the slow command components (shortest-path for delta). */
    float dd = delta_t - TrackDelta;
    const float pi = 3.14159265359f;
    const float two_pi = 6.28318530718f;
    if (dd > pi) dd -= two_pi;
    if (dd < -pi) dd += two_pi;
    const float d_max = DeltaSlewRps * Dt;
    TrackDelta += (dd > d_max) ? d_max : ((dd < -d_max) ? -d_max : dd);
    const float m_max_step = MSlew * Dt;
    const float dm = m_t - TrackM;
    TrackM += (dm > m_max_step) ? m_max_step : ((dm < -m_max_step) ? -m_max_step : dm);

    const float ang = Theta_E + TrackDelta;

    /* Pulse number with change-dwell: apply only after the new value has
     * persisted, so transients cannot chatter the slot pattern. */
    const float ratio = FAsyncHz / F_Elec;
    int n_new = 3 * (2 * (int)(ratio / 6.0f) + 1);
    if (n_new < 3) n_new = 3;
    if (n_new > 999) n_new = 999;
    int N = (NState > 0.5f) ? (int)NState : n_new;
    if (n_new != N) {
        NChangeT += Dt;
        if (NChangeT * 1000.0f >= NHoldMs) {
            N = n_new;
            NChangeT = 0.0f;
        }
    } else {
        NChangeT = 0.0f;
    }
    NState = (float)N;
    N_Pulses = (float)N;

    const float h = two_pi / (float)N;
    const float sinc = (h > 1.0e-6f) ? (sinf(0.5f * h) / (0.5f * h)) : 1.0f;

    /* Slot-averaged cosine reference. */
    auto slotavg = [&](float phase) -> float {
        const float slot = floorf(phase / h);
        return cosf((slot + 0.5f) * h) * sinc;
    };

    auto duty = [](float d) -> float {
        if (d < 0.0f) return 0.0f;
        if (d > 100.0f) return 100.0f;
        return d;
    };

    /* Phase references in per-unit-of-vdc, then the SAME min-max
     * zero-sequence injection as Transforms.Svpwm.  Shape-matching SVPWM at
     * the ladder boundary is not optional: blending an injected waveform
     * against a plain clamped cosine differs by ~10% duty at m~0.9, which
     * is ~3 V of error for the whole crossfade and kicked the bench 370 A. */
    const float inv_sqrt3 = 0.57735026919f;
    const float ra = TrackM * inv_sqrt3 * slotavg(ang);
    const float rb = TrackM * inv_sqrt3 * slotavg(ang - 2.09439510239f);
    const float rc = TrackM * inv_sqrt3 * slotavg(ang + 2.09439510239f);
    const float v_min = fminf(ra, fminf(rb, rc));
    const float v_max = fmaxf(ra, fmaxf(rb, rc));
    const float v_offset = 0.5f * (v_min + v_max);

    Duty_A = duty(50.0f + 50.0f * (ra - v_offset));
    Duty_B = duty(50.0f + 50.0f * (rb - v_offset));
    Duty_C = duty(50.0f + 50.0f * (rc - v_offset));
}
