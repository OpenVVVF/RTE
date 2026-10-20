/* Sin/cos encode from true rotor angle (twin/plant semantics):
 *   phi_mech = (ThetaE - OffsetDeg_rad) / Poles
 *   sin_c = Mid + Amp*sin(phi), cos_c = Mid + Amp*cos(phi)
 * Disconnect rails both counts to 0 (hard pull). */
if (Disconnect >= 0.5) {
    Sin = 0.0;
    Cos = 0.0;
} else {
    double phi = ((double)ThetaE - (double)OffsetDeg * (double)(M_PI / 180.0)) / (double)Poles;
    Sin = floor((double)MidSin + (double)AmpSin * sin(phi) + 0.5);
    Cos = floor((double)MidCos + (double)AmpCos * cos(phi) + 0.5);
}
