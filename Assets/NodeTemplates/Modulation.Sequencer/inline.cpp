/* Railway-style modulation sequencer (mode ladder + boundary table).
 *
 * v2 ladder: 0 = async carrier (SVPWM), 1 = synchronous sampled PWM,
 * 2 = direct-state SHE (PatternModulator).  The ladder is data: each
 * boundary has an enter threshold, an exit threshold (hysteresis), a
 * minimum dwell, and a transition type.  Extending the ladder adds
 * boundary params, not new logic.
 *
 * Transition mechanics:
 *  - 0 <-> 1 (both duty-cycle modes): Blend ramps over TransMs (duty
 *    crossfade; both modulators are shape-matched so it is benign).
 *  - 1 <-> 2 (duty path <-> pattern hardware): hard switch.  The SHE
 *    node arms the PatternModulator phase-locked to the commanded vector
 *    on the ModeTo edge; Blend2 steps in the same step.
 *
 * Blend  = weight of mode 1 in the (0,1) pair (0 = SVPWM duties).
 * Blend2 = weight of mode 2 in the (1,2) pair (routing only; the pattern
 *          driver owns the pins once enabled). */

const bool en = Enable;

if (!en || !(Dt > 0.0f)) {
    CurMode = 0.0f;
    DwellT = 0.0f;
    TransT = 0.0f;
    BlendS = 0.0f;
    ModeFrom = 0.0f;
    ModeTo = 0.0f;
    Blend = 0.0f;
    Blend2 = 0.0f;
    Mode = 0.0f;
    TransActive = false;
} else if (TransT > 0.0f) {
    /* Transition in progress. */
    TransActive = true;
    const bool duty_pair = (ModeFrom < 1.5f && ModeTo < 1.5f);
    if (TransMs > 0.0f && duty_pair) {
        /* Duty crossfade on the 0 <-> 1 boundary. */
        const float dir = (ModeTo > ModeFrom) ? 1.0f : -1.0f;
        BlendS += dir * Dt / (TransMs * 0.001f);
        if (BlendS > 1.0f) BlendS = 1.0f;
        if (BlendS < 0.0f) BlendS = 0.0f;
        if ((dir > 0.0f && BlendS >= 1.0f) || (dir < 0.0f && BlendS <= 0.0f)) {
            TransT = 0.0f;
        }
    } else {
        /* Hard switch (1 <-> 2 boundary, or TransMs == 0). */
        BlendS = (ModeTo >= 1.0f) ? 1.0f : 0.0f;
        TransT = 0.0f;
    }
    if (TransT == 0.0f) {
        CurMode = ModeTo;
        ModeFrom = CurMode;
        ModeTo = CurMode;
        TransActive = false;
    }
    Blend = BlendS;
    Blend2 = (CurMode >= 1.5f || (TransT > 0.0f && ModeTo >= 1.5f)) ? 1.0f : 0.0f;
    Mode = CurMode;
} else {
    /* Steady state: watch the adjacent boundaries. */
    ModeFrom = CurMode;
    ModeTo = CurMode;
    BlendS = (CurMode >= 0.5f) ? 1.0f : 0.0f;
    Blend = BlendS;
    Blend2 = (CurMode >= 1.5f) ? 1.0f : 0.0f;
    Mode = CurMode;
    TransActive = false;

    if (CurMode < 0.5f) {
        DwellT = (F_Elec >= FEnterHz) ? (DwellT + Dt) : 0.0f;
        if (DwellT * 1000.0f >= MinDwellMs) {
            DwellT = 0.0f;
            TransT = Dt;
            ModeFrom = 0.0f;
            ModeTo = 1.0f;
        }
    } else if (CurMode < 1.5f) {
        if (F_Elec >= FEnter2Hz && Allow2) {
            DwellT += Dt;
            if (DwellT * 1000.0f >= MinDwellMs) {
                DwellT = 0.0f;
                TransT = Dt;
                ModeFrom = 1.0f;
                ModeTo = 2.0f;
            }
        } else if (F_Elec <= FExitHz) {
            DwellT += Dt;
            if (DwellT * 1000.0f >= MinDwellMs) {
                DwellT = 0.0f;
                TransT = Dt;
                ModeFrom = 1.0f;
                ModeTo = 0.0f;
            }
        } else {
            DwellT = 0.0f;
        }
    } else {
        DwellT = (F_Elec <= FExit2Hz) ? (DwellT + Dt) : 0.0f;
        if (DwellT * 1000.0f >= MinDwellMs) {
            DwellT = 0.0f;
            TransT = Dt;
            ModeFrom = 2.0f;
            ModeTo = 1.0f;
        }
    }
}
