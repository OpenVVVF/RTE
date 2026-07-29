#!/usr/bin/env bash
# Build and run Icarus Verilog sims (fail-fast).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
mkdir -p build/sim

echo "==> tb_pwm"
iverilog -g2005-sv -o build/sim/tb_pwm.vvp \
  rtl/deadtime_pair.v rtl/pwm_complementary.v tb/tb_pwm.v
( cd build/sim && vvp tb_pwm.vvp )

echo "==> tb_spi"
iverilog -g2005-sv -o build/sim/tb_spi.vvp \
  rtl/spi_regs.v tb/tb_spi.v
( cd build/sim && vvp tb_spi.vvp )

echo "ALL SIMS PASSED"
