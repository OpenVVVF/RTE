/* Throttle channels normalized by Hw.ThrA/B.MinV/MaxV. Hw.ThrB.Enable=0
 * selects A only and makes B zero. In dual mode a sustained mismatch
 * makes both normalized outputs zero and Valid false. */
A = platform_get_throttle_a();
B = platform_get_throttle_b();
Valid = platform_get_throttle_valid();
/* Raw PA3 pin voltage for bounds measurement, before normalization/clamping. */
A_V = au::volts(platform_get_throttle_a_voltage());
