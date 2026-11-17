# FPGA 顶层：Block Design mca_block（Vivado IP Integrator）。本脚本生成工程 build/mca_block/mca_block.xpr
#   在 fpga/ 下：vivado -mode batch -nojournal -nolog -source scripts/block_design.tcl [-tclargs sim]
# 单独运行只建工程，之后可以用 Vivado 打开看图、在 GUI 里综合实现；scripts/build.tcl 先运行本脚本再编译出位流。
# sim：给 sim/run_sim.py 用，建 build/mca_sim/（心跳 1 ms、PHY 复位 100 拍，加速仿真）并导出 xsim 文件列表。
#
# 本脚本就是顶层的源码：工程每次重新生成，在 GUI 里改了图要把改动写回这里，否则下次编译会被覆盖。
# 各模块以 Module Reference 方式引用 rtl/ 的源码，只改模块内部不用动这里。
# 导出图（docs/mca_block.pdf）：GUI 里打开 Block Design，在 Tcl Console 运行
#   write_bd_layout -format pdf -orientation portrait <文件>.pdf
#
# Vivado 按系统编码（GBK）读脚本，所以代码行和输出提示只用英文。
set here [file normalize [file dirname [info script]]]
set root [file normalize $here/..]
set rtl  $root/rtl
set for_sim [expr {[info exists argv] && "sim" in $argv}]
set name [expr {$for_sim ? "mca_sim" : "mca_block"}]
set proj $root/build/$name

create_project -force $name $proj -part xc7a35tfgg484-2
# mca_regs.vh 在 eth_core、adc_domain 的模块体里 include，要加进工程并设好包含路径
add_files [concat [glob $rtl/*.v] $rtl/mca_regs.vh]
set_property include_dirs $rtl [current_fileset]
if {!$for_sim} { add_files -fileset constrs_1 $root/constr/dafenqi_pro.xdc }
create_bd_design mca_block

proc cell {name ref {cfg {}}} {
    set c [create_bd_cell -type module -reference $ref $name]
    if {[llength $cfg]} { set_property -dict $cfg $c }
}
proc ip {name vlnv {cfg {}}} {
    set c [create_bd_cell -type ip -vlnv $vlnv $name]
    if {[llength $cfg]} { set_property -dict $cfg $c }
}
proc port {name dir {w 1} {type {}} args} {
    set a [list -dir $dir {*}$args]
    if {$w > 1} { lappend a -from [expr {$w - 1}] -to 0 }
    if {$type ne {}} { lappend a -type $type }
    create_bd_port {*}$a $name
}
proc net {name args} {
    set objs {}
    foreach p $args {
        if {[string first / $p] < 0} { lappend objs [get_bd_ports $p] } else { lappend objs [get_bd_pins $p] }
    }
    connect_bd_net -net $name {*}$objs
}

# 心跳周期、PHY 复位保持时间（125 MHz 拍数）：上板 1 s、20 ms；仿真 1 ms、100 拍
set hb_cycles  [expr {$for_sim ? 125000 : 125000000}]
set phy_cycles [expr {$for_sim ? 100 : 2500000}]

# 时钟：ADC_CLK_PHASE 是 ad_clk 相对内部采样沿的相位（度，步进 1.8）
cell u_clk     clk_gen    {CONFIG.ADC_CLK_PHASE 180.0}
# 复位：复位键 + 时钟锁定 → 每个时钟域一个同步复位（高有效），PHY 复位再多保持 phy_cycles。
# 复位键低有效，Processor System Reset 从 sys_rst_n 端口的 POLARITY 自动取极性
ip   u_rst_eth xilinx.com:ip:proc_sys_reset:5.0
ip   u_rst_adc xilinx.com:ip:proc_sys_reset:5.0
cell u_phy_rst phy_rst    [list CONFIG.CYCLES $phy_cycles]
cell u_adc     adc_if
cell u_adcd    adc_domain
cell u_ev_fifo async_fifo {CONFIG.DW 72 CONFIG.AW 10}
# RGMII：IDELAY_VALUE 是 RX 数据延时抽头（78 ps/抽头，按 RGMII-ID 最坏窗口居中），TXC_90 让 TXC 相移 90 度
cell u_rgmii   rgmii_if   {CONFIG.IDELAY_VALUE 10 CONFIG.TXC_90 1}
cell u_core    eth_core   [list CONFIG.HB_CYCLES $hb_cycles CONFIG.CYC_PER_MS 125000]
cell u_led     led_drv

# 对外引脚（名字和约束文件一致）
port sys_clk   I 1 clk -freq_hz 50000000
port sys_rst_n I 1 rst
set_property CONFIG.POLARITY ACTIVE_LOW [get_bd_ports sys_rst_n]
port ad_clk O;          port ad_data I 8;   port ad_otr I
port eth_rxc I 1 clk -freq_hz 125000000
port eth_rx_ctl I;      port eth_rxd I 4
port eth_txc O;         port eth_tx_ctl O;  port eth_txd O 4
port eth_rst_n O;       port led O 4

# 时钟与复位
net sys_clk     sys_clk u_clk/sys_clk
net sys_rst_n   sys_rst_n u_clk/sys_rst_n u_rst_eth/ext_reset_in u_rst_adc/ext_reset_in
net clk_eth     u_clk/clk_eth u_rst_eth/slowest_sync_clk u_phy_rst/clk u_led/clk \
                u_ev_fifo/rd_clk u_rgmii/clk_eth u_core/clk u_adcd/rd_clk
net clk_eth90   u_clk/clk_eth90 u_rgmii/clk_eth90
net clk_ref     u_clk/clk_ref u_rgmii/clk_ref
net clk_adc     u_clk/clk_adc u_rst_adc/slowest_sync_clk u_adc/clk_adc u_adcd/clk u_ev_fifo/wr_clk
net clk_adc_fwd u_clk/clk_adc_fwd u_adc/clk_adc_fwd
net locked      u_clk/locked u_rst_eth/dcm_locked u_rst_adc/dcm_locked
net rst_eth     u_rst_eth/peripheral_reset u_phy_rst/rst u_ev_fifo/rd_rst u_rgmii/rst_eth u_core/rst
net rst_adc     u_rst_adc/peripheral_reset u_adcd/rst u_ev_fifo/wr_rst

# ADC
net ad_clk      u_adc/ad_clk ad_clk
net ad_data     ad_data u_adc/ad_data
net ad_otr      ad_otr u_adc/ad_otr
net adc_sample  u_adc/sample u_adcd/adc_sample
net adc_otr_q   u_adc/otr u_adcd/adc_otr

# ADC 时钟域与以太网核心之间：配置、操作握手、计数快照、波形读口（两边引脚同名）
foreach s {cfg_bus op_tgl op_code op_arg op_ack_tgl snap_total_events snap_pileup snap_dropped snap_sat
           snap_total_ticks snap_busy_ticks snap_baseline snap_ts snap_flags wave_done_tgl wave_start wave_depth
           wave_pretrig wave_ts wave_src wave_rd_addr wave_rd_data} {
    net $s u_adcd/$s u_core/$s
}

# 事件 FIFO
net ev_valid    u_adcd/ev_valid u_ev_fifo/wr_en
net ev_data     u_adcd/ev_data u_ev_fifo/din
net ev_full     u_ev_fifo/full u_adcd/ev_full
net ev_rd       u_core/ev_rd u_ev_fifo/rd_en
net ev_dout     u_ev_fifo/dout u_core/ev_dout
net ev_empty    u_ev_fifo/empty u_core/ev_empty

# 以太网
net eth_rxc     eth_rxc u_rgmii/rgmii_rxc
net eth_rx_ctl  eth_rx_ctl u_rgmii/rgmii_rx_ctl
net eth_rxd     eth_rxd u_rgmii/rgmii_rxd
net eth_txc     u_rgmii/rgmii_txc eth_txc
net eth_tx_ctl  u_rgmii/rgmii_tx_ctl eth_tx_ctl
net eth_txd     u_rgmii/rgmii_txd eth_txd
net rx_rd_en    u_core/rx_rd_en u_rgmii/rx_rd_en
net rx_dout     u_rgmii/rx_dout u_core/rx_dout
net rx_empty    u_rgmii/rx_empty u_core/rx_empty
net tx_en       u_core/tx_en u_rgmii/tx_en
net tx_data     u_core/tx_data u_rgmii/tx_data
net byte_en     u_rgmii/byte_en u_core/byte_en
net speed       u_rgmii/speed u_core/speed u_led/speed

# PHY 复位与指示灯
net eth_rst_n   u_phy_rst/phy_rst_n eth_rst_n u_led/phy_ready
net led_run     u_core/led_run u_led/led_run
net led_event   u_adcd/led_event u_led/led_event
net led         u_led/led led

# 自己的模块不该有悬空引脚（u_core/led_host 本来就不用）
foreach p [get_bd_pins -of [get_bd_cells -filter {VLNV =~ *:module_ref:*}]] {
    if {![llength [get_bd_nets -quiet -of $p]]} { puts "unconnected pin: $p" }
}
regenerate_bd_layout
validate_bd_design
save_bd_design

set bd [get_files mca_block.bd]
# 整个设计一起综合，不给两个复位 IP 单独跑 OOC
set_property synth_checkpoint_mode None $bd
generate_target all $bd
add_files -norecurse [make_wrapper -files $bd -top]
set_property top mca_block_wrapper [current_fileset]
update_compile_order -fileset sources_1

if {$for_sim} {
    export_ip_user_files -of_objects $bd -no_script -sync -force -quiet
    export_simulation -of_objects $bd -simulator xsim -directory $proj/export -force -quiet
}
puts "Project ready: $proj/$name.xpr"
