/* DutyIn is a host-shimmed external: the emitter declares
 * state.<NodeId>_Du/Dv/Dw as real externals the host assigns per control
 * period; this body just passes them through so downstream references are
 * consistent. (Nothing here - emitter wires the extern directly.) */
