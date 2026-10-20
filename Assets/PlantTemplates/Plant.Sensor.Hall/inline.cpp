/* Hall transducer: offset + LSB quantization + range clamp; on Rail >= 0.5
 * the recorded isolated-rail-collapse artifact (RailVal_A) is emitted
 * verbatim, bypassing quantization AND the range clamp (recorded artifact
 * values sit outside the healthy range, twin/plant _rail_val). */
if (Rail >= 0.5) {
    I_Meas = (double)RailVal_A;
} else {
    const double x = I_True + Offset_A;
    /* nearest-even ties (rint): matches python round() at exact half-LSB */
    I_Meas = fmin((double)Range_A, fmax(-(double)Range_A, rint(x / (double)Lsb_A) * (double)Lsb_A));
}
