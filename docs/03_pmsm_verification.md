# PMSM Verification

## RTE status

No differential-equation PMSM plant exists in upstream RTE. Motor dynamics on hardware are physical; parameters come from `cal Motor.PMSM.*`.

## New plant model (`PmsmPlant.h`)

Standard dq equations (SPMSM/IPMSM):

\[
v_d = R_s i_d + L_d \frac{di_d}{dt} - \omega_e L_q i_q
\]
\[
v_q = R_s i_q + L_q \frac{di_q}{dt} + \omega_e L_d i_d + \omega_e \psi_f
\]
\[
T_e = \frac{3}{2}p\left[\psi_f i_q + (L_d - L_q)i_d i_q\right]
\]
\[
J\frac{d\omega_m}{dt} = T_e - T_L - B\omega_m,\quad \omega_e = p\omega_m
\]

Default parameters from Zhang et al. Table I (2.4 kW machine).

### Verification results (all pass)

| Test | Result |
|------|--------|
| Zero-voltage current decay | PASS |
| Standstill d-axis response | PASS |
| Standstill q-axis response | PASS |
| Back-EMF at nonzero speed | PASS |
| SPM torque vs i_q | PASS |
| Mechanical acceleration | PASS |
| ω_e = p·ω_m | PASS |
| Electrical angle integration | PASS |
| vs high-resolution reference integrator | PASS (tol 0.05 A) |

Run: `./build/Lib/Simulation/Simulation_pmsm_tests`

## No defects found requiring RTE model changes

The absence of a plant model is a **missing feature**, not a bug in existing RTE code.
