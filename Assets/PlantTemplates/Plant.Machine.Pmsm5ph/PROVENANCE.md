# Provenance — Plant.Machine.Pmsm5ph model simplifications

This template ships the 5-phase rig as an **honest subset**: correct
topology and plane treatment, with every non-proven term listed here
rather than silently absorbed. No extra axis is "faked": the z1/z2 axes
are integrated physics with their own parameters, and everything they
exclude is named below.

## What is physics (follows `twin/plant/plant.py` / PmsmDq exactly)

- dq fundamental-plane PMSM equations (Ld, Lq, Rs, Lambda, pole pairs),
  the cross-coupling/back-EMF terms, and the angle integration, including
  the same derivative ordering and output-from-final-state rules.
- Knee distortion stays on the inverter node, not here.
- OpenGates freewheel collapse (tau = 2 ms) applied to all current axes,
  same twin convention as the 3-phase machine.
- Torque/power: `Te = (5/2)*Poles*(Lambda*Iq + (Ld - Lq)*Id*Iq)`,
  `P_Elec = Te * Omega/Poles` — the N-phase air-gap power convention
  (P = (N/2) sum over the dq winding power with peak per-phase units,
  which reduces exactly to the shipped 3/2 factor at N = 3).

## Stated approximations (calibration gaps, not silent cuts)

1. **z1/z2 rotor/mutual coupling neglected.** The z plane is an R/L pair
   per axis (`diz = (vz - Rs*iz)/Lz`). This is exact for an ideal
   sinusoidal-distributed 5-phase machine with no 3rd-harmonic EMF
   content; any real rotor z-plane saliency is unmodeled.
2. **Common-mode (zs) axis not integrated.** An open-star five-phase
   machine has no common-mode current path, and Leg5ph removes the
   common mode per substep by construction (`d_avg` over five legs), so
   zs is identically zero in this model. Star-point displacement voltage
   from knee distortion residuals is ignored in the averaged view.
3. **Lz is a calibration-gap parameter** — no recorded 5-phase bench
   evidence exists for this parametrization yet (class: a per-phase
   leakage of the same order as Ld/Lq).
4. **Five-phase knee evidence absent.** Leg5ph reuses the 3-phase
   twin-calibrated distortion law per leg; see its inline.cpp note.

## Registry status (why `rte sim` cannot run this yet)

Plant node kinds live in the engines' classify tables
(`Lib/PlantCodegen/src/PlantGenerator.cpp` `Classify`, mirrored by
`twin/plantgraph/plantgraph.py` `NODE_CLASSES`), and the C++ emitter
hard-rejects loaded types missing from the table (contract D3). This
wave owns assets only, so neither table was edited: both new templates
are load-proven through `NodeAPI::LoadNodeTypesFromDirectory` and the
rig graph is structurally validated, but closed-loop `rte sim` of the
rig waits on the ONE registry line per engine (kind-in-schema, twin
ROADMAP M6, is the declared v2 fix). A contract-faithful evaluator
mirroring the two bodies drives the scripted-dyno evidence run in the
meantime (`/tmp/house_five_phase_rig/`, numbers in the wave handoff).
