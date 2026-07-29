# Flash / scope notes for humans (agent gate is simulation).

## Sim gate (required before claiming FPGA work done)

```powershell
cd Fpga\TangNano20k
.\scripts\build_sim.ps1
```

Must print `ALL SIMS PASSED` and exit 0.

## Gowin EDA — program Tang Nano 20K

1. Install [Gowin EDA](https://www.gowinsemi.com/en/support/download_eda/) (Windows).
2. New Project → device **GW2AR-LV18QN88C8/I7** (GW2AR-18C).
3. Add all `rtl/*.v` and `constraints/tangnano20k.cst`. Top: `tangnano20k_top`.
4. Run Synthesis → Place & Route.
5. Programmer: USB-C to board (BL616). Download bitstream to **external Flash** (not SRAM-only) so it persists.
6. Press KEY2 after config if `rst_n` is held; LED0 should stay off until SPI enables PWM.

## Scope bring-up (no MCU required)

1. Probe `pwm_uh`/`pwm_ul` (pins 25/26). With defaults after SPI enable: ~10 kHz, ~50 % high-side, ~1 µs both-off dead-time.
2. Enable via any Mode-0 SPI master (logic analyzer / USB-SPI / Nucleo later):
   - Write `CTRL=0x0001` at addr `0x04`
   - Optionally set `DUTY_U/V/W`, `FREQ_HZ`, `DEADTIME_NS` (see `docs/register_map.md`)
3. Confirm complementary non-overlap on each pair before connecting gate drivers.

## Optional open-source flow

- Yosys + nextpnr-himbaechel/gowin + Apicula (`openFPGALoader -b tangnano20k`).
- Not required for this track; Gowin GUI is primary.
