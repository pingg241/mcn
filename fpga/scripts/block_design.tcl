# 生成 Block Design 工程（Vivado IP Integrator）：build/mca_block/mca_block.xpr
#   在 fpga/ 下：vivado -mode batch -nojournal -nolog -source scripts/block_design.tcl
# 然后用 Vivado 打开这个工程 → IP INTEGRATOR → Open Block Design。导出图（docs/mca_block.pdf 就是这么来的）：
#   write_bd_layout -format pdf -orientation portrait <文件>.pdf
#
# 只用来看结构、出图，不用来编译：真正的顶层是 rtl/mca_top.v，位流用 build.tcl 生成。
# 各模块以 Module Reference 方式引用 rtl/ 里的源码，连线和 mca_top.v 一一对应；mca_top.v 顶层里那几行
# 零散逻辑（复位合成、PHY 复位延时、指示灯）在这里包成 top_glue 方块。u_core/led_host 在 mca_top.v 里
# 也没接，所以图上悬空。改了 RTL 就重新运行本脚本，或在图上右键 Refresh Changed Modules。
#
# Vivado 按系统编码（GBK）读脚本，所以下面写进 top_glue.v 的代码和输出提示只用英文。
set here [file normalize [file dirname [info script]]]
set root [file normalize $here/..]
set rtl  $root/rtl
set proj $root/build/mca_block
file mkdir $proj

set fh [open $proj/top_glue.v w]
puts $fh {`timescale 1ns / 1ps
// Block-design view only: the glue logic that sits directly in mca_top.v
// (reset merge, PHY reset delay, LEDs), wrapped as one block.
module top_glue #(parameter PHY_RST_CYCLES = 2_500_000) (
    input            sys_rst_n,
    input            locked,
    output           arst,
    input            clk_eth,
    input            rst_eth,
    output reg       eth_rst_n = 1'b0,
    input            led_event,
    input            led_run,
    input      [1:0] speed,
    output     [3:0] led
);
    assign arst = ~sys_rst_n | ~locked;
    reg [22:0] phy_cnt = 0;
    always @(posedge clk_eth) begin
        if (rst_eth) begin
            phy_cnt <= 0; eth_rst_n <= 1'b0;
        end else if (phy_cnt < PHY_RST_CYCLES) begin
            phy_cnt <= phy_cnt + 1'b1; eth_rst_n <= 1'b0;
        end else begin
            eth_rst_n <= 1'b1;
        end
    end
    reg [26:0] blink = 0;
    always @(posedge clk_eth) blink <= blink + 1'b1;
    assign led = {led_event, led_run, (speed != 2'd0), blink[26] & eth_rst_n};
endmodule}
close $fh

# mca_top.v 不加进来：async_fifo 在它下面有两种参数化，Module Reference 会认不出该用哪个
create_project -force mca_block $proj -part xc7a35tfgg484-2
add_files [concat [lsearch -all -inline -not [glob $rtl/*.v] *mca_top.v] $rtl/mca_regs.vh $proj/top_glue.v]
set_property include_dirs $rtl [current_fileset]
update_compile_order -fileset sources_1
create_bd_design mca_block

proc cell {name ref {cfg {}}} {
    set c [create_bd_cell -type module -reference $ref $name]
    if {[llength $cfg]} { set_property -dict $cfg $c }
}
proc port {name dir {w 1} {type {}}} {
    set a [list -dir $dir]
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

# 模块（实例名和 mca_top.v 一致）
cell u_clk     clk_gen
cell u_rs_eth  rst_sync
cell u_rs_adc  rst_sync
cell u_glue    top_glue
cell u_adc     adc_if
cell u_adcd    adc_domain
cell u_ev_fifo async_fifo {CONFIG.DW 72 CONFIG.AW 10}
cell u_rgmii   rgmii_if
cell u_core    eth_core

# 对外引脚
port sys_clk I 1 clk;   port sys_rst_n I 1 rst
port ad_clk O 1 clk;    port ad_data I 8;   port ad_otr I
port eth_rxc I 1 clk;   port eth_rx_ctl I;  port eth_rxd I 4
port eth_txc O 1 clk;   port eth_tx_ctl O;  port eth_txd O 4
port eth_rst_n O;       port led O 4

# 时钟与复位
net sys_clk     sys_clk u_clk/sys_clk
net sys_rst_n   sys_rst_n u_clk/sys_rst_n u_glue/sys_rst_n
net clk_eth     u_clk/clk_eth u_rs_eth/clk u_glue/clk_eth u_ev_fifo/rd_clk u_rgmii/clk_eth u_core/clk u_adcd/rd_clk
net clk_eth90   u_clk/clk_eth90 u_rgmii/clk_eth90
net clk_ref     u_clk/clk_ref u_rgmii/clk_ref
net clk_adc     u_clk/clk_adc u_rs_adc/clk u_adc/clk_adc u_adcd/clk u_ev_fifo/wr_clk
net clk_adc_fwd u_clk/clk_adc_fwd u_adc/clk_adc_fwd
net locked      u_clk/locked u_glue/locked
net arst        u_glue/arst u_rs_eth/arst u_rs_adc/arst
net rst_eth     u_rs_eth/rst u_glue/rst_eth u_ev_fifo/rd_rst u_rgmii/rst_eth u_core/rst
net rst_adc     u_rs_adc/rst u_adcd/rst u_ev_fifo/wr_rst

# ADC
net ad_clk      u_adc/ad_clk ad_clk
net ad_data     ad_data u_adc/ad_data
net ad_otr      ad_otr u_adc/ad_otr
net adc_sample  u_adc/sample u_adcd/adc_sample
net adc_otr_q   u_adc/otr u_adcd/adc_otr

# ADC 时钟域 ↔ 以太网核心：配置、操作握手、计数快照、波形读口（两边引脚同名）
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
net speed       u_rgmii/speed u_core/speed u_glue/speed

# 指示灯与 PHY 复位
net led_event   u_adcd/led_event u_glue/led_event
net led_run     u_core/led_run u_glue/led_run
net eth_rst_n   u_glue/eth_rst_n eth_rst_n
net led         u_glue/led led

foreach p [get_bd_pins -of [get_bd_cells]] {
    if {![llength [get_bd_nets -quiet -of $p]]} { puts "unconnected pin: $p" }
}
regenerate_bd_layout
save_bd_design
close_project
puts "Block design ready: open $proj/mca_block.xpr"
