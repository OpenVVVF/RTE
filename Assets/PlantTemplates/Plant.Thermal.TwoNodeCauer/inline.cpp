/* Two-node Cauer chain + NTC spot-offset view (twin/plant semantics):
 *   dT1 = (P - (T1-T2)/R12) / C1
 *   dT2 = ((T1-T2)/R12 - (T2-TAmb)/R2) / C2
 */
T1 += (P_Heat - (T1 - T2) / (double)R12_KW) / (double)C1_JK * DtPlant;
T2 += ((T1 - T2) / (double)R12_KW - (T2 - (double)TAmb_C) / (double)R2_KW)
      / (double)C2_JK * DtPlant;

T_Plate = T1;
T_Sink = T2;
T_Meas1 = T1 + Off1;
/* ch2 NTC law (twin/plant/plant.py idle-warm artifact):
 *   t_plate + Off2Idle - Off2Slope * max(0, t_plate - (t_amb + Off2Knee)) */
T_Meas2 = T1 + Off2Idle
        - (double)Off2Slope * fmax(0.0, T1 - ((double)TAmb_C + (double)Off2Knee_C));
/* ch3: constant third spot (three-strip assemblies like the DSR bike rig) */
T_Meas3 = T1 + Off3;
