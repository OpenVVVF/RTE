/* Free shaft inertia: J dot(omega_mech)/dt = Torque - B*omega_mech
 * (state in RPM for readability; the table-lookup math: d(Rpm)/dt =
 * (Te - B*(K*Rpm)) / (J*K)+. */
const double K = 2.0 * M_PI / 60.0;
Rpm += ((double)Torque - (double)B * (K * Rpm)) / ((double)J) * (DtPlant / K);
Omega_Elec = Rpm * K * Poles;
Elec_Rpm = Rpm * Poles;
