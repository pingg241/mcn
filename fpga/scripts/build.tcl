# 全流程：建工程（block_design.tcl）→ 综合 → 实现 → 位流，产物 build/mca.bit、build/mca.bin
#   在 fpga/ 下：vivado -mode batch -source scripts/build.tcl
#   可选参数：-tclargs synth   只综合
# 跑完后也可以用 Vivado 打开 build/mca_block/mca_block.xpr 看综合、实现结果。
set here [file normalize [file dirname [info script]]]
source $here/block_design.tcl
set out $root/build
set only_synth [expr {"synth" in $argv}]

proc run_ok {run} {
    if {[get_property PROGRESS [get_runs $run]] ne "100%"} { error "$run failed, see [get_property DIRECTORY [get_runs $run]]" }
}

launch_runs synth_1 -jobs 4
wait_on_run synth_1
run_ok synth_1
open_run synth_1
report_utilization -file $out/synth_util.rpt
if {$only_synth} {
    report_timing_summary -file $out/synth_timing.rpt
    exit
}
close_design

set_property STEPS.PHYS_OPT_DESIGN.IS_ENABLED true [get_runs impl_1]
launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1
run_ok impl_1
open_run impl_1

report_utilization       -file $out/util.rpt
report_utilization -hierarchical -hierarchical_depth 4 -file $out/util_hier.rpt
report_timing_summary -max_paths 20 -report_unconstrained -file $out/timing.rpt
report_clock_interaction -file $out/clock_interaction.rpt
report_cdc               -file $out/cdc.rpt
report_drc               -file $out/drc.rpt
report_io                -file $out/io.rpt

set wns [get_property SLACK [get_timing_paths -max_paths 1 -nworst 1 -setup]]
set whs [get_property SLACK [get_timing_paths -max_paths 1 -nworst 1 -hold]]
puts "TIMING: WNS=$wns WHS=$whs"

file copy -force [get_property DIRECTORY [get_runs impl_1]]/mca_block_wrapper.bit $out/mca.bit
# SPI Flash 固化用（达芬奇Pro 板载 16 MB QSPI）
write_cfgmem -force -format bin -interface SPIx4 -size 16 -loadbit "up 0x0 $out/mca.bit" -file $out/mca.bin
