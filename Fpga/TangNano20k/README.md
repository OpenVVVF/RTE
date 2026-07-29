# Tang Nano 20K — standalone FPGA PWM track

> **Toolchain:** [Icarus Verilog](http://bleyer.org/icarus/) is **required** for the RTL simulation gate (`scripts/build_sim.*` must exit 0). **Gowin EDA** is the recommended path for synthesis, place/route, and programming on real hardware.

Complete **FPGA-only** workflow for Sipeed **Tang Nano 20K** (`GW2AR-LV18QN88C8/I7` / GW2AR-18C).  
Does **not** depend on Gen6FW, `Lib/`, or `Source/`. The SPI register map in [`docs/register_map.md`](docs/register_map.md) is the frozen contract for a later Nucleo `SPI2` join.

## Tree

```
Fpga/TangNano20k/
├── README.md
├── rtl/                 # Verilog (PWM, SPI slave, FOC stubs, top)
├── tb/                  # Icarus testbenches (must exit 0)
├── constraints/         # tangnano20k.cst
├── scripts/             # build_sim, flash notes, optional Gowin tcl
└── docs/
    └── register_map.md  # SPI contract
```

## What it does

| Block | Role |
|-------|------|
| `pwm_complementary` | 6-channel center-aligned complementary PWM + dead-time (TIM1 electrical intent: ~10 kHz, ~1 µs DT, 50 % park) |
| `spi_regs` | Mode-0 SPI slave + register file (duty U/V/W, freq, dead-time, enable, fault/status, magic/version) |
| `foc_*_stub` | Empty Clarke / Park / SVPWM shells — **not** in the PWM path |
| `tangnano20k_top` | Wiring for board pins |

Fixed-frequency / fixed-duty defaults come from the register file reset values; change them over SPI (or edit defaults in `spi_regs.v`).

## Simulation gate (required)

Install [Icarus Verilog](http://bleyer.org/icarus/) (or oss-cad-suite). Then:

```powershell
cd Fpga\TangNano20k
.\scripts\build_sim.ps1
```

- `tb_pwm` — duty, period, dead-time, complementary non-overlap  
- `tb_spi` — simulated SPI master, full register map  

Both must print `PASS` and the script must exit **0**.

Optional Verilator: compile the same RTL/TB sources with `--timing` / event-driven TB support; Icarus is the checked path.

## Gowin EDA — manual hardware test

1. Install **Gowin V1.9+** from Gowin Semiconductor.
2. **New Project** → device **GW2AR-LV18QN88C8/I7** (device version **C**).
3. Add sources:
   - `rtl/deadtime_pair.v`
   - `rtl/pwm_complementary.v`
   - `rtl/spi_regs.v`
   - `rtl/foc_clarke_stub.v`
   - `rtl/foc_park_stub.v`
   - `rtl/foc_svpwm_stub.v`
   - `rtl/tangnano20k_top.v`
   - `constraints/tangnano20k.cst`
4. Set top module `tangnano20k_top`.
5. Run **Synthesize** → **Place & Route**.
6. **Program Device** over USB-C (onboard BL616). Program **external Flash** so the bitstream survives power cycle.
7. Scope bring-up (no MCU):
   - Hold KEY2 released (`rst_n` high).
   - Drive SPI (Mode 0): write `CTRL=1` (`addr 0x04`) to enable PWM.
   - Probe pins **25/26** (UH/UL): expect ~10 kHz, ~50 % high, ~1 µs both-low dead-time.
8. Details: [`scripts/flash_notes.md`](scripts/flash_notes.md). Optional Tcl sketch: [`scripts/gowin_project.tcl`](scripts/gowin_project.tcl).

### Pin summary

| Signal | FPGA pin | Notes |
|--------|----------|--------|
| `clk_27m` | 4 | 27 MHz OSC |
| `rst_n` | 87 | KEY2, active-low |
| `led_n` | 15 | LED0, active-low; on when PWM running |
| `pwm_uh..wl` | 25–30 | LCD FPC GPIOs (LCD unused) |
| `spi_cs_n/sclk/mosi/miso` | 71–74 | Edge connector for later Nucleo SPI2 |

## Optional open-source flow (Yosys / nextpnr / Apicula)

Not required if Gowin is used. Typical outline:

```text
yosys -p "read_verilog rtl/*.v; synth_gowin -top tangnano20k_top -json top.json"
nextpnr-himbaechel --json top.json --write pnr.json --device GW2AR-18C --vopt family=GW2A-18C --vopt cst=constraints/tangnano20k.cst
gowin_pack -d GW2AR-18C -o top.fs pnr.json
openFPGALoader -b tangnano20k top.fs
```

Tool versions and Himbaechel flags change often — treat Gowin GUI as the supported path for bring-up.

## Compatibility

- Stays under `Fpga/TangNano20k/` only.
- No dependency on MCU firmware images.
- Later join: Nucleo SPI2 master ↔ this slave using `docs/register_map.md` only.
