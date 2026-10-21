/* SPMSM dq-Euler (non-salient): identical form to Plant.Machine.PmsmDq with
 * Ld == Lq == L_H, torque strictly 1.5*pp*Lambda*iq. Only `DtPlant` + ports
 * + parameters are runtime symbols; ordering rules per the README ground
 * rules (derivatives pre-update; abc rebuilt from the final state). */

double ct = cos((double)ThetaE);
double st = sin((double)ThetaE);

double valpha = (2.0 / 3.0) * (V_U - 0.5 * (V_V + V_W));
double vbeta  = (2.0 / 3.0) * (M_SQRT3 / 2.0) * (V_V - V_W);
double vd = ct * valpha + st * vbeta;
double vq = -st * valpha + ct * vbeta;

if (OpenGates >= 0.5) {
    /* six-switch-open freewheel collapse (~1-2 ms evidence window) */
    double tau_open = 2.0e-3;
    Id -= Id * DtPlant / tau_open;
    Iq -= Iq * DtPlant / tau_open;
} else {
    double did = (vd - Rs * Id + Omega_Elec * L_H * Iq) / L_H;
    double diq = (vq - Rs * Iq - Omega_Elec * (L_H * Id + Lambda)) / L_H;
    Id += did * DtPlant;
    Iq += diq * DtPlant;
}
ThetaE = fmod((double)ThetaE + Omega_Elec * DtPlant, 2.0 * M_PI);
if (ThetaE < 0.0) ThetaE += 2.0 * M_PI;

Id = fmin(2000.0, fmax(-2000.0, Id));
Iq = fmin(2000.0, fmax(-2000.0, Iq));

Torque = 1.5 * Poles * Lambda * Iq;    // SPMSM: no reluctance term exists
P_Elec = (OpenGates >= 0.5) ? 0.0 : Torque * Omega_Elec / Poles;

double ct2 = cos((double)ThetaE);
double st2 = sin((double)ThetaE);
I_U = ct2 * Id - st2 * Iq;
I_V = cos((double)ThetaE - 2.0 * M_PI / 3.0) * Id - sin((double)ThetaE - 2.0 * M_PI / 3.0) * Iq;
I_W = -I_U - I_V;
