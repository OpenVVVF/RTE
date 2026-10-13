#pragma once

/**
 * Gen6 bench motor defaults matching the working FOC graph / calibration notes.
 * Sources:
 *  - Assets/Examples/foc_demo.json (Dt, PI gains)
 *  - Assets/Examples/mpcc_demo.json (Rs/Ld/Lq placeholders + KV keys)
 *  - Images/Gen6FW/reminder.md (Ld/Lq/flux order of magnitude, 10 poles)
 *
 * Used only by the local FOC-vs-MPC paper comparison (does not touch RTE git).
 */

#include "simulation/FocController.h"
#include "simulation/MpccController.h"
#include "simulation/PmsmPlant.h"

namespace simulation {

inline PmsmParameters makeGen6BenchMotor() {
    PmsmParameters m;
    m.rs = 0.05f;           // Motor.Resistance.Avg default
    m.ld = 70e-6f;          // ~70 µH
    m.lq = 120e-6f;         // ~120 µH
    m.psi_f = 0.072f;       // flux-cal ~0.07–0.08 Wb
    m.pole_pairs = 5;       // Motor.Poles = 10
    m.inertia = 2.5e-4f;    // small bench rotor (simulation)
    m.viscous_friction = 5.0e-4f;
    return m;
}

/** FOC current-loop gains for a stable working Gen6-like current loop.
 *  foc_demo.json stores Kp=0.03 / Ki=10 (very soft). With calibrated flux
 *  (~0.072 Wb) and Vdc needed for 2000 rpm, those defaults lose current
 *  regulation as back-EMF grows. Here we use a moderate bandwidth PI that
 *  still spins the motor cleanly — slower than deadbeat MPC, as in the
 *  FOC-vs-MPC comparison literature. */
inline FocParameters makeGen6FocParams(const PmsmParameters& motor, float ts = 200e-6f) {
    FocParameters p;
    p.motor = motor;
    p.ts = ts;
    // ωc ≈ 2π·250 rad/s → Kp~L·ωc, Ki~R·ωc (classic PMSM PI sizing)
    p.kp_d = 0.12f;
    p.ki_d = 80.0f;
    p.kp_q = 0.18f;
    p.ki_q = 80.0f;
    p.aw_gain = 1.0f;
    return p;
}

inline MpccParameters makeGen6MpccParams(const PmsmParameters& motor, float ts = 200e-6f,
                                         MPCCMode mode = MPCCMode::OptimalDutyCycle) {
    MpccParameters p;
    p.motor = motor;
    p.ts = ts;
    p.i_base = 20.0f;
    p.i_max = 40.0f;
    p.mode = mode;
    return p;
}

}  // namespace simulation
