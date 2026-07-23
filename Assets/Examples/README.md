# RTE Examples

This directory holds example node graphs that can be fed to `RTECodeEmitter`.

## `foc_chain.json`

A complete field-oriented control chain for a three-phase PMSM/BLDC inverter. It
runs entirely in the `isr_pwm` timing domain and uses the templates in
`Assets/NodeTemplates/`.

### Signal flow

```
hw.adc.phase_currents  -->  math.clarke  -->  math.park
                                                    ^
                                                    | theta (constant)
                                                    v
control.pi_current (d) <-- math.dq_unpack <--   i_dq
control.pi_current (q) <-- math.dq_unpack
        |                               |
        v                               v
   math.dq_pack  -->  math.inverse_park  -->  math.inverse_clarke
        ^                                       |
        | theta (constant)                      | (optional abc voltage)
        |
   math.svpwm  <--  vdc (constant)
        |
        v
   hw.pwm.set_duty
```

### Running it

```bash
cd /home/aidan/Desktop/RTE
./build/Source/RTECodeEmitter/RTECodeEmitter \
  --base-src Images/Gen6FW \
  --graph Assets/Examples/foc_chain.json \
  --output /tmp/foc_out \
  --templates Assets/NodeTemplates
```

The `--templates` flag is required because the graph references template types
(`hw.adc.phase_currents`, `math.clarke`, etc.) instead of defining them inline.
