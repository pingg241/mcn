# 非工程模式全流程：综合 → 实现 → 位流
#   vivado -mode batch -source fpga/scripts/build.tcl
#   可选参数：-tclargs synth   只综合
set here [file normalize [file dirname [info script]]]
set root [file normalize $here/..]
set out  $root/build
set part xc7a35tfgg484-2
file mkdir $out

set only_synth [expr {[llength $argv] > 0 && [lindex $argv 0] eq "synth"}]

read_verilog [glob $root/rtl/*.v]
read_xdc $root/constr/dafenqi_pro.xdc
synth_design -top mca_top -part $part -include_dirs $root/rtl -flatten_hierarchy rebuilt
report_utilization -file $out/synth_util.rpt
if {$only_synth} {
    report_timing_summary -file $out/synth_timing.rpt
    exit
}

opt_design
place_design
phys_opt_design
route_design

report_utilization       -file $out/util.rpt
report_utilization -hierarchical -hierarchical_depth 3 -file $out/util_hier.rpt
report_timing_summary -max_paths 20 -report_unconstrained -file $out/timing.rpt
report_clock_interaction -file $out/clock_interaction.rpt
report_cdc               -file $out/cdc.rpt
report_drc               -file $out/drc.rpt
report_io                -file $out/io.rpt

set wns [get_property SLACK [get_timing_paths -max_paths 1 -nworst 1 -setup]]
set whs [get_property SLACK [get_timing_paths -max_paths 1 -nworst 1 -hold]]
puts "TIMING: WNS=$wns WHS=$whs"

write_checkpoint -force $out/mca_top_routed.dcp
write_bitstream  -force $out/mca_top.bit
# SPI Flash 固化用（达芬奇Pro 板载 16 MB QSPI）
write_cfgmem -force -format bin -interface SPIx4 -size 16 -loadbit "up 0x0 $out/mca_top.bit" -file $out/mca_top.bin
