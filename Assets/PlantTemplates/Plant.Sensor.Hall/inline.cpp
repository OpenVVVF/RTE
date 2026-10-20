/* Hall transducer: offset + LSB quantization + range clamp; optional
 * phantom rail when the isolated rail dies (post-fault capture artifact). */
double x = I_True + Offset_A;
if (Rail >= 0.5) {
    x = (Offset_A >= 0.0) ? 0.98 * Range_A : -0.98 * Range_A;   // rails with sign of offset
}
I_Meas = fmin((double)Range_A, fmax(-(double)Range_A, round(x / (double)Lsb_A) * (double)Lsb_A));
