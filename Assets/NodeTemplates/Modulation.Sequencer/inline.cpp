/* Railway-style modulation sequencer (mode ladder + boundary table).
 *
 * v1 ladder: 0 = carrier (SVPWM), 1 = six-step.  The ladder is data: each
 * boundary has an enter threshold, an exit threshold (hysteresis), a minimum
 * dwell, and a transition type (TransMs == 0: hard phase-locked switch;
 * TransMs > 0: timed linear crossfade).  Extending the ladder (sync-15/12/9/
 * 7/5/3, overmodulation, ...) adds boundary params, not new logic.
 *
 * Continuity at the switch is the modulators' job (gain/phase-matched arm),
 * not this node's.  This node only decides WHEN and reports From/To/Blend.
 *
 * Blend is the output weight of the higher ladder slot: 0 = mode 0 duties,
 * 1 = mode 1 duties.  Steady state: Blend == CurMode. */

const bool en = Enable;

if (!en || !(Dt > 0.0f)) {
    CurMode = 0.0f;
    DwellT = 0.0f;
    TransT = 0.0f;
    BlendS = 0.0f;
    ModeFrom = 0.0f;
    ModeTo = 0.0f;
    Blend = 0.0f;
    Mode = 0.0f;
    TransActive = false;
} else if (TransT > 0.0f) {
    /* Transition in progress. */
    TransActive = true;
    if (TransMs > 0.0f) {
        const float dir = (ModeTo > ModeFrom) ? 1.0f : -1.0f;
        BlendS += dir * Dt / (TransMs * 0.001f);
        if (BlendS > 1.0f) BlendS = 1.0f;
        if (BlendS < 0.0f) BlendS = 0.0f;
        if ((dir > 0.0f && BlendS >= 1.0f) || (dir < 0.0f && BlendS <= 0.0f)) {
            TransT = 0.0f;
        }
    } else {
        /* Hard switch: the incoming modulator armed on the ModeTo edge last
         * step and is gain/phase-matched; switch now. */
        BlendS = ModeTo;
        TransT = 0.0f;
    }
    if (TransT == 0.0f) {
        CurMode = ModeTo;
        ModeFrom = CurMode;
        ModeTo = CurMode;
        TransActive = false;
    }
    Blend = BlendS;
    Mode = CurMode;
} else {
    /* Steady state: watch the adjacent boundary. */
    ModeFrom = CurMode;
    ModeTo = CurMode;
    BlendS = CurMode;
    Blend = BlendS;
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
    } else {
        DwellT = (F_Elec <= FExitHz) ? (DwellT + Dt) : 0.0f;
        if (DwellT * 1000.0f >= MinDwellMs) {
            DwellT = 0.0f;
            TransT = Dt;
            ModeFrom = 1.0f;
            ModeTo = 0.0f;
        }
    }
}
