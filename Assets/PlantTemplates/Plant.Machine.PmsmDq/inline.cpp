/* PMSM dq-Euler integration (exactly twin/plant/plant.py semantics).
 * Runtime symbols: only parameters/substituted ports and `DtPlant` (the host
 * substep). `OpenGates` is an optional input port (default 0). */

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
    Id += (vd - Rs * Id + Omega_Elec * Lq * Iq) / Ld * DtPlant;
    Iq += (vq - Rs * Iq - Omega_Elec * (Ld * Id + Lambda)) / Lq * DtPlant;
}
ThetaE = fmod((double)ThetaE + Omega_Elec * DtPlant, 2.0 * M_PI);
if (ThetaE < 0.0) ThetaE += 2.0 * M_PI;

// physical state rails (matches twin/plant.py)
Id = fmin(2000.0, fmax(-2000.0, Id));
Iq = fmin(2000.0, fmax(-2000.0, Iq));

Torque = 1.5 * Poles * (Lambda * Iq + (Ld - Lq) * Id * Iq);
P_Elec = Torque * Omega_Elec / Poles;   // air-gap power (plant convention)

// abc outputs from the final (id, iq, theta) this step (sensor instant)
I_U = ct * Id - st * Iq;
I_V = cos((double)ThetaE - 2.0 * M_PI / 3.0) * Id - sin((double)ThetaE - 2.0 * M_PI / 3.0) * Iq;
I_W = -I_U - I_V;
