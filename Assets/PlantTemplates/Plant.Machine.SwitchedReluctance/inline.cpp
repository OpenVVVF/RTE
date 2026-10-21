/* Switched reluctance machine: analytic macroscopic model (twin/plantgraph
 * class mirror). psi(i, theta) from aligned/unaligned curves with a
 * (1+cos(Nr*theta))/2 tooth profile; per-phase Kirchhoff integration with
 * the position-dependent incremental inductance and motion back-EMF; torque
 * is the analytic coenergy derivative (finite-difference validated to 1e-4).
 * Omega_Elec carries MECHANICAL rad/s in this template. */

/* aligned-phase flux map and its inductance:
 *   lambda_a(i) = LambdaSat * (1 - e^{-Ka*i}) ;
 *   d lambda_a/di = LambdaSat*Ka * e^{-Ka*i}        (L_unsat at i=0)     */
double la = 0.0, dla = 0.0;

for (int j = 0; j < 3; ++j) {
    double i = (j == 0) ? I_U : (j == 1) ? I_V : I_W;
    double v = (j == 0) ? V_U : (j == 1) ? V_V : V_W;
    double phi = j * (2.0 * M_PI) / (3.0 * RotorTeeth);
    double x = RotorTeeth * (ThetaE - phi);
    double f  = (1.0 + cos(x)) / 2.0;
    double fp = -RotorTeeth * sin(x) / 2.0;
    la  = LambdaSat * (1.0 - exp(-Ka * i));
    dla = LambdaSat * Ka * exp(-Ka * i);
    if (OpenGates >= 0.5) {
        double tau_open = 2.0e-3;
        i -= i * DtPlant / tau_open;
    } else {
        double l_inc = LU_H + (dla - LU_H) * f;
        double bemf  = (la - LU_H * i) * fp * Omega_Elec;
        double di    = (v - Rs * i - bemf) / fmax(l_inc, 1e-6);
        i += di * DtPlant;
    }
    i = fmin(2000.0, fmax(-2000.0, i));
    if (j == 0) I_U = i; else if (j == 1) I_V = i; else I_W = i;
}

ThetaE = fmod((double)ThetaE + Omega_Elec * DtPlant, 2.0 * M_PI);
if (ThetaE < 0.0) ThetaE += 2.0 * M_PI;

/* analytic coenergy torque over the phases */
Torque = 0.0;
PsiAligned = 0.0;
{
    double i_max = 0.0;
    for (int j = 0; j < 3; ++j) {
        double i = (j == 0) ? I_U : (j == 1) ? I_V : I_W;
        double phi = j * (2.0 * M_PI) / (3.0 * RotorTeeth);
        double x = RotorTeeth * (ThetaE - phi);
        double fp = -RotorTeeth * sin(x) / 2.0;
        double integ = LambdaSat * (i - (1.0 - exp(-Ka * i)) / Ka)
                     - LU_H * i * i / 2.0;
        Torque += fp * integ;
        i_max = fmax(i_max, i);
    }
    PsiAligned = LambdaSat * (1.0 - exp(-Ka * i_max));

    /* instantaneous winding input power; zero while gates open */
    P_Elec = (OpenGates >= 0.5) ? 0.0
                                : V_U * I_U + V_V * I_V + V_W * I_W;
}
