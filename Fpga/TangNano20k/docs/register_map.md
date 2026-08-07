# Tang Nano 20K PWM — SPI Register Map

**Frozen contract** for a later join with Nucleo `SPI2` (MCU master → FPGA slave).  
This FPGA track does **not** depend on the MCU image; the map is the only intended software interface.

## Physical / framing

| Item | Value |
|------|--------|
| Mode | SPI Mode 0 (`CPOL=0`, `CPHA=0`) |
| Bit order | MSB first |
| Frame | 24 bits: `ADDR[7:0]` then `DATA[15:0]` |
| Chip select | Active-low `CS_N`; idle high between frames |
| Clock | Master-driven; recommend ≤ 10 MHz for bring-up |
| Voltage | 3.3 V LVCMOS |

### Address byte

| Bits | Meaning |
|------|---------|
| `[7]` | `0` = write, `1` = read |
| `[6:0]` | Register address |

### Write

1. Drive `CS_N` low.  
2. Clock out `{0, addr[6:0]}`, then `data[15:8]`, then `data[7:0]`.  
3. Drive `CS_N` high. Write commits on the last SCLK rising edge while `CS_N` is low (or on `CS_N` rising — slave latches on bit 23 complete).

### Read

1. Drive `CS_N` low.  
2. Clock out `{1, addr[6:0]}`, then two dummy bytes.  
3. On `MISO`, after the address byte, the slave returns `data[15:8]` then `data[7:0]`.  
4. Drive `CS_N` high.

`MISO` is high-Z when `CS_N` is high.

## Register table

All registers are 16-bit. Unlisted addresses read as `0x0000` and ignore writes.

| Addr | Name | Access | Reset | Description |
|------|------|--------|-------|-------------|
| `0x00` | `MAGIC` | RO | `0x544E` | ASCII `"TN"` — presence check |
| `0x01` | `VERSION` | RO | `0x0001` | Register-map / RTL version |
| `0x02` | `STATUS` | RO | `0x0000` | See status bits below |
| `0x03` | `FAULT` | R / W1C | `0x0000` | Sticky faults; write-1-to-clear per bit |
| `0x04` | `CTRL` | RW | `0x0000` | Control bits |
| `0x05` | `FREQ_HZ` | RW | `10000` | Switching frequency (Hz), center-aligned |
| `0x06` | `DEADTIME_NS` | RW | `1000` | Dead-time (nanoseconds), same electrical intent as MCU TIM1 (~1 µs) |
| `0x07` | `DUTY_U` | RW | `5000` | Phase U duty, units of 0.01 % (`0`…`10000` → 0…100.00 %) |
| `0x08` | `DUTY_V` | RW | `5000` | Phase V duty, same units |
| `0x09` | `DUTY_W` | RW | `5000` | Phase W duty, same units |
| `0x0A` | `SCRATCH` | RW | `0x0000` | Loopback / connectivity test |
| `0x0B` | `CLK_HZ_LO` | RO | — | System clock Hz, low 16 bits |
| `0x0C` | `CLK_HZ_HI` | RO | — | System clock Hz, high 16 bits |

### `STATUS` bits

| Bit | Name | Meaning |
|-----|------|---------|
| 0 | `ENABLED` | `CTRL.PWM_EN` is set and outputs are armed |
| 1 | `FAULT` | Any sticky bit in `FAULT` is set |
| 2 | `RUNNING` | PWM timer is counting (enabled and no blocking fault) |
| 15:3 | — | Reserved `0` |

### `CTRL` bits

| Bit | Name | Meaning |
|-----|------|---------|
| 0 | `PWM_EN` | `1` = enable complementary PWM outputs; `0` = force all six outs low |
| 15:1 | — | Reserved; write `0` |

### `FAULT` bits (W1C)

| Bit | Name | Meaning |
|-----|------|---------|
| 0 | `BAD_FREQ` | Requested `FREQ_HZ` out of range / period overflow |
| 1 | `BAD_DUTY` | Any duty > `10000` (clamped; sticky set) |
| 2 | `BAD_DEAD` | Dead-time longer than half period (clamped; sticky set) |
| 15:3 | — | Reserved |

## Electrical defaults (MCU TIM1 intent)

| Parameter | Default | Notes |
|-----------|---------|--------|
| Topology | 3× complementary pairs (UH/UL, VH/VL, WH/WL) | Same six gate signals as TIM1 CH1/1N/2/2N/3/3N |
| Alignment | Center-aligned | Matches TIM1 `CENTERALIGNED1` |
| Frequency | 10 kHz | Matches Gen6FW open-loop / reminder guidance |
| Dead-time | 1000 ns | Matches `PWM_SetDeadTime(1000)` / CubeMX ~1 µs |
| Duty | 50 % each phase | Safe idle park |

Duty is applied to the **high-side** ideal pulse; low-side is complementary with dead-time on both rising edges (non-overlap required).

## Versioning

- Bump `VERSION` when this document or the slave address decode changes.  
- Hosts must check `MAGIC == 0x544E` before trusting other registers.
