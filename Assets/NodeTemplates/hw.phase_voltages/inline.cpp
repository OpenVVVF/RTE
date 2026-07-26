/* All voltage sense channels from the MAX22530 isolated ADC, filtered. */
V_U  = rte::Volts(platform_phase_voltage_u());
V_V  = rte::Volts(platform_phase_voltage_v());
V_W  = rte::Volts(platform_phase_voltage_w());
V_Dc = rte::Volts(platform_get_dc_link_voltage());
