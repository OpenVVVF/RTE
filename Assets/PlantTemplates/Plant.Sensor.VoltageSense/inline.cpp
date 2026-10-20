/* Isolated voltage sense: first-order lag then LSB quantization
 * (matches twin/plant's MAX22530 path). */
Lag += ((double)V_True - Lag) * fmin(1.0, (double)DtPlant / (double)LagTau_S);
V_Meas = round(Lag / (double)Lsb_V) * (double)Lsb_V;
