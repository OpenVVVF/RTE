/* Dual throttle channels, normalized [0..1] by the base-image driver
 * (KV calibration Hw.ThrA/B.MinV/MaxV).  Both read 0 and Valid is false
 * while the channels disagree beyond the plausibility tolerance. */
A = platform_get_throttle_a();
B = platform_get_throttle_b();
Valid = platform_get_throttle_valid();
