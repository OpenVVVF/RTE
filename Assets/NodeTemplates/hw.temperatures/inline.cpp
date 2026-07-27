/* Motor + board temperatures from the base-image ApplicationSensors driver.
 * NAN while a channel is disabled or out of range (open/short). */
T_Motor = rte::Celsius(platform_get_motor_temperature());
T_Inv1  = rte::Celsius(platform_get_inverter_temperature(0));
T_Inv2  = rte::Celsius(platform_get_inverter_temperature(1));
T_Inv3  = rte::Celsius(platform_get_inverter_temperature(2));
