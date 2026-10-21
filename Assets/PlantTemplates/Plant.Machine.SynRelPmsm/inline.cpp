/* PMa-SyRM (hybrid PM-assisted synchronous reluctance) dq-Euler integration.
 * Physics is byte-equivalent to Plant.Machine.PmsmDq (twin/plant/plant.py
 * semantics): the hybrid is the standard PM-in-a-salient-frame form; the
 * reluctance product (Ld-Lq)*Id*Iq carries most of the torque in SyRM class
 * machines. This header comment only is different - never edit the equations
 * away from PmsmDq's.
 * Runtime symbols: only parameters/substituted ports and `DtPlant` (the host
 * substep). `OpenGates` is an optional input port (default 0).
 *
 * Ordering rules (validated by twin/plantgraph/proof_bench_parity.py):
 *   - ALL derivatives are computed from the PRE-update state, then assigned
 *     (assigning Id before computing dIq breaks bit-comparability), and
 *   - abc/torque outputs are rebuilt from the FINAL (id, iq, theta) of the
 *     substep (the plant measurement view is instantaneous with the
 *     reported state), and
 *   - P_Elec is zero when the gates are open (the collapse returns energy
 *     via the machine state decay itself, not the air-gap power term).
 */

double ct = cos((double)ThetaE);
double st = sin((double)ThetaE);

double valpha = (2.0 / 3.0) * (V_U - 0.5 * (V_V + V_W));
double vbeta  = (2.0 / 3.0) * (M_SQRT3 / 2.0) * (V_V - V_W);
double vd = ct * valpha + st * vbeta;
double vq = -st * valpha + ct * vbeta;

if (OpenGates >= 0.5) {
    /* six-switch-open: antiparallel diodes only conduct toward the link and
     * only when EMF overcomes it; the twin models the whole open state as an
     * exponential freewheel collapse (~1-2 ms), matching recorded telemetry
     * windows of the gate-kill tests (20/21). */
    double tau_open = 2.0e-3;
    Id -= Id * DtPlant / tau_open;
    Iq -= Iq * DtPlant / tau_open;
} else {
    double did = (vd - Rs * Id + Omega_Elec * Lq * Iq) / Ld;
    double diq = (vq - Rs * Iq - Omega_Elec * (Ld * Id + Lambda)) / Lq;
    Id += did * DtPlant;
    Iq += diq * DtPlant;
}
ThetaE = fmod((double)ThetaE + Omega_Elec * DtPlant, 2.0 * M_PI);
if (ThetaE < 0.0) ThetaE += 2.0 * M_PI;

// physical state rails (matches twin/plant.py)
Id = fmin(2000.0, fmax(-2000.0, Id));
Iq = fmin(2000.0, fmax(-2000.0, Iq));

Torque = 1.5 * Poles * (Lambda * Iq + (Ld - Lq) * Id * Iq);
P_Elec = (OpenGates >= 0.5) ? 0.0 : Torque * Omega_Elec / Poles;

// abc outputs from the FINAL (id, iq, theta) this step (sensor instant)
double ct2 = cos((double)ThetaE);
double st2 = sin((double)ThetaE);
I_U = ct2 * Id - st2 * Iq;
I_V = cos((double)ThetaE - 2.0 * M_PI / 3.0) * Id - sin((double)ThetaE - 2.0 * M_PI / 3.0) * Iq;
I_W = -I_U - I_V;
