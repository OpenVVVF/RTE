# Motor Type and Calibration Commands

## MotorType values

`MotorType` is stored as a `uint32_t` in FRAM and in the runtime `MotorCalibration`
struct.  The calibration dispatcher uses it to decide which parameter-identification
routine is appropriate for the connected motor.

| Value | Name        | Meaning                                          |
|-------|-------------|--------------------------------------------------|
| 0     | `Unknown`   | Not configured.  Treated as an error for FOC.    |
| 1     | `PmsmIpm`   | Interior permanent-magnet synchronous motor.     |
| 2     | `PmsmSpm`   | Surface permanent-magnet synchronous motor.      |
| 3     | `Induction` | Squirrel-cage or wound-rotor induction machine.  |
| 4     | `SynRel`    | Synchronous reluctance motor (future).           |
| 5     | `Brushed`   | Brushed DC motor (future).                       |
| 6     | `SlipRing`  | Wound-rotor synchronous motor (future).          |

Both `PmsmIpm` (1) and `PmsmSpm` (2) count as **PMSM** for the hierarchical `cal`
command.  Everything else is rejected by the PMSM-specific routines.

## Setting and inspecting the motor type

The type is set from the shell with the numeric value:

```
setconfig type 1      # PMSM_IPM
setconfig type 2      # PMSM_SPM
setconfig type 3      # Induction
```

Or by name:

```
setconfig type pmsm_ipm
setconfig type pmsm_spm
setconfig type induction
```

The active runtime type is shown by:

```
cal status
```

## Phase-wire swap

If the motor leads are plugged in the wrong order, the drive can electronically
swap two phases instead of rewiring.  The swap is applied to both the PWM
voltage outputs and the phase-current feedback, so the FOC algorithm still sees
a consistent UVW coordinate system.

| Value | Swap | Example wiring |
|-------|------|----------------|
| 0     | none | UVW (default)  |
| 1     | U↔V  | VUW            |
| 2     | V↔W  | UWV            |
| 3     | U↔W  | WVU            |

Set from the shell:

```
config set Motor.PhaseSwap 1
```

The value is read from FRAM at boot, so a power cycle is required after changing
it.  Use `0` for normal wiring.

## Calibration command families

The `cal` command is now strictly partitioned by motor family.  Running a routine
on the wrong motor family is refused with an error instead of silently changing
the motor type.

### PMSM routines

Require `motor_type` to be `PmsmIpm` (1) or `PmsmSpm` (2):

- `cal Motor.PMSM` — full PMSM profile: Ld/Lq then flux linkage.
- `cal Motor.PMSM.Inductance [I_A]` — biased-AC injection Ld/Lq only.
- `cal Motor.PMSM.FluxLinkage` — back-EMF flux-linkage sweep only.

### Induction routines

Require `motor_type` to be `Induction` (3):

- `cal Motor.Induction [I_A]` — standstill sigma-Ls / rotor-time-constant test.
- `cal Motor.Induction.VHz [f] [--wye]` — encoderless V/Hz spin Ls sweep.

### Shared routines

These do not depend on motor family:

- `cal Motor.Poles`
- `cal Motor.Encoder[.SinCos[.Breakaway]]`
- `cal Motor.Encoder.Linearity`
- `cal Motor.Resistance [I_A]`

### Full automatic profile

`cal all` reads the current `motor_type` and branches accordingly:

- PMSM: pole → encoder → resistance → Ld/Lq → flux linkage.
- Induction: pole → encoder → resistance → sigma-Ls / tau_r.

## Why the strict checks matter

Previously, `cal Motor.Induction` and `cal Motor.Induction.VHz` silently overwrote
`motor_type` to `Induction` if it was set to something else.  This made it easy to
accidentally run induction-specific parameter identification on a PMSM, which then
caused the auto-coordinator to skip the PMSM Ld/Lq and flux-linkage stages on the
next `cal all` or `cal Motor.PMSM` invocation.

The V/Hz inductance sweep in particular is **not** valid for PMSMs: the routine
assumes the current is nearly 90° out of phase with the voltage (magnetizing
current), which is only true for an induction machine near no-load slip.  A PMSM
at the default 10 Hz is resistance-dominated, so the phase check rejects the data
and the hunt fails.  PMSM inductance should be measured with
`cal Motor.PMSM.Inductance`, which uses locked-rotor AC current injection and does
not depend on back-EMF or V/Hz sync.
