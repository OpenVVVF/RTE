/* Synchronous n-pulse PWM (synchronized sampled sine) - the ladder's
 * intermediate sync mode.
 *
 * The pattern is locked to the rotor angle: duties follow a sine referenced
 * to Theta_E + atan2(V_Q, V_D), slot-averaged over 2*pi/N slots, with N
 * chosen from the chopping-rate budget (largest odd multiple of 3 with
 * N*f_e <= FAsyncHz).  The timer keeps chopping at the async carrier, so
 * phase current ripple stays carrier-bounded at ANY fundamental; only the
 * average staircase is rotor-locked.  At high N (low f_e) this converges to
 * async SVPWM; as f_e rises N steps down and switching becomes visibly
 * synchronous.
 *
 * NOT six-step: full-rail plateaus on a low-L motor at low f_e let phase
 * current free-run between edges (bench: 250 A at 20 Hz).  Square-wave SHE
 * patterns belong to the top of the ladder (voltage ceiling), later.
 *
 * Contract: ModeId-gated, holds 50% neutral otherwise; vdc < 1 V -> 50%. */

const float vdc = V_Dc.in(au::volts);
const bool running = (ModeId == ModeFrom) || (ModeId == ModeTo);

if (!running || !(vdc > 1.0f) || !(F_Elec > 0.01f)) {
    Duty_A = 50.0f;
    Duty_B = 50.0f;
    Duty_C = 50.0f;
} else {
    const float vd = V_D.in(au::volts);
    const float vq = V_Q.in(au::volts);

    /* Rotor-locked reference angle: theta_e + smooth dq phase offset. */
    const float ang = Theta_E + atan2f(vq, vd);

    /* Commanded magnitude relative to the SVPWM linear max (vdc/sqrt(3));
     * the 2/sqrt(3) factor makes the phase fundamental match SVPWM's. */
    float m = sqrtf(vd * vd + vq * vq) / (vdc * 0.57735026919f);
    if (m > 1.0f) m = 1.0f;
    if (m < 0.0f) m = 0.0f;
    const float g = 1.1547005384f * m;  /* 2/sqrt(3) * m */

    /* Pulse number: largest odd multiple of 3 within the chopping budget.
     * Steps up freely, steps down with margin (hysteresis). */
    const float ratio = FAsyncHz / F_Elec;
    int n_new = 3 * (2 * (int)(ratio / 6.0f) + 1);
    if (n_new < 3) n_new = 3;
    if (n_new > 999) n_new = 999;
    int N = (NState > 0.5f) ? (int)NState : n_new;
    if (n_new > N) N = n_new;
    else if (n_new < N - 6) N = n_new;
    NState = (float)N;
    N_Pulses = (float)N;

    const float two_pi = 6.28318530718f;
    const float h = two_pi / (float)N;
    const float sinc = (h > 1.0e-6f) ? (sinf(0.5f * h) / (0.5f * h)) : 1.0f;

    /* Slot-averaged reference: average value of the phase reference over
     * each slot, so the held duty matches the slot's mean.  The reference
     * is cos (NOT sin): SVPWM's phase A follows valpha = |V|*cos(phi), so
     * phase A must peak when the vector points at +alpha.  Using sin here
     * rotated the applied vector 90 deg into the d-axis and blew up the
     * bench (id > 400 A, bus collapse). */
    auto slotavg = [&](float phase) -> float {
        const float slot = floorf(phase / h);
        return cosf((slot + 0.5f) * h) * sinc;
    };

    auto duty = [](float d) -> float {
        if (d < 0.0f) return 0.0f;
        if (d > 100.0f) return 100.0f;
        return d;
    };

    Duty_A = duty(50.0f + 50.0f * g * slotavg(ang));
    Duty_B = duty(50.0f + 50.0f * g * slotavg(ang - 2.09439510239f));
    Duty_C = duty(50.0f + 50.0f * g * slotavg(ang + 2.09439510239f));
}
