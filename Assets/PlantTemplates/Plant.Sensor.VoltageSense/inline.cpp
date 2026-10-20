/* Isolated voltage sense: first-order lag then LSB quantization
 * (matches twin/plant's MAX22530 path). LagTau_S <= 0 = no lag (bus channel
 * on the 8 kHz AA filter is effectively lag-free at control rates). */
if (LagTau_S > 0.0) {
    Lag += ((double)V_True - Lag) * fmin(1.0, (double)DtPlant / (double)LagTau_S);
} else {
    Lag = (double)V_True;
}
V_Meas = rint(Lag / (double)Lsb_V) * (double)Lsb_V;   // nearest-even ties == python round()
