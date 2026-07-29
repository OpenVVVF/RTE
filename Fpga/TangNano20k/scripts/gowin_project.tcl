# Optional Gowin IDE TCL sketch (run inside Gowin Tcl console after creating project).
# Primary flow is still manual GUI — see README.md.
#
# Example:
#   cd Fpga/TangNano20k
#   # open Gowin → Tcl Console → source scripts/gowin_project.tcl

set_device -name GW2AR-18C -device_version C
# Adjust path to your Gowin project once created.
# add_file -type verilog rtl/deadtime_pair.v
# add_file -type verilog rtl/pwm_complementary.v
# add_file -type verilog rtl/spi_regs.v
# add_file -type verilog rtl/foc_clarke_stub.v
# add_file -type verilog rtl/foc_park_stub.v
# add_file -type verilog rtl/foc_svpwm_stub.v
# add_file -type verilog rtl/tangnano20k_top.v
# add_file -type cst constraints/tangnano20k.cst
# run all
# # then Program Device → external Flash (bitstream store)
