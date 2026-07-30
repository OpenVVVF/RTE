/* User digital output. Pin 1..4 -> USER_DOUT_1..4, 5 -> green LED,
 * 6 -> orange LED (see platform_api.h). */
platform_digital_write(static_cast<uint8_t>(Pin), In);
