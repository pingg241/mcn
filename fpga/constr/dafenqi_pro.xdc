## 正点原子达芬奇Pro（XC7A35T-2FGG484）+ ATK-HS-AD/DA（插 J3）+ 核心板网口 GE_0
## 引脚来源：两份达芬奇Pro 公开工程（其中一份在 GE_0 上板实测），上板前请对照手头板卡原理图核对。

## ---------------- 时钟 / 复位 / LED ----------------
create_clock -period 20.000 -name sys_clk [get_ports sys_clk]
set_property -dict {PACKAGE_PIN R4 IOSTANDARD LVCMOS15} [get_ports sys_clk]
set_property -dict {PACKAGE_PIN U7 IOSTANDARD LVCMOS15 PULLUP TRUE} [get_ports sys_rst_n]

set_property -dict {PACKAGE_PIN V9 IOSTANDARD LVCMOS15} [get_ports {led[0]}]
set_property -dict {PACKAGE_PIN Y8 IOSTANDARD LVCMOS15} [get_ports {led[1]}]
set_property -dict {PACKAGE_PIN Y7 IOSTANDARD LVCMOS15} [get_ports {led[2]}]
set_property -dict {PACKAGE_PIN W7 IOSTANDARD LVCMOS15} [get_ports {led[3]}]

## ---------------- AD9280（ATK-HS-AD/DA，J3） ----------------
set_property -dict {PACKAGE_PIN B22 IOSTANDARD LVCMOS33} [get_ports {ad_data[7]}]
set_property -dict {PACKAGE_PIN C22 IOSTANDARD LVCMOS33} [get_ports {ad_data[6]}]
set_property -dict {PACKAGE_PIN B21 IOSTANDARD LVCMOS33} [get_ports {ad_data[5]}]
set_property -dict {PACKAGE_PIN A21 IOSTANDARD LVCMOS33} [get_ports {ad_data[4]}]
set_property -dict {PACKAGE_PIN D14 IOSTANDARD LVCMOS33} [get_ports {ad_data[3]}]
set_property -dict {PACKAGE_PIN D15 IOSTANDARD LVCMOS33} [get_ports {ad_data[2]}]
set_property -dict {PACKAGE_PIN E16 IOSTANDARD LVCMOS33} [get_ports {ad_data[1]}]
set_property -dict {PACKAGE_PIN D16 IOSTANDARD LVCMOS33} [get_ports {ad_data[0]}]
set_property -dict {PACKAGE_PIN C20 IOSTANDARD LVCMOS33} [get_ports ad_otr]
set_property -dict {PACKAGE_PIN D20 IOSTANDARD LVCMOS33 SLEW FAST DRIVE 8} [get_ports ad_clk]

## ---------------- RGMII（GE_0） ----------------
set_property -dict {PACKAGE_PIN U20  IOSTANDARD LVCMOS33} [get_ports eth_rxc]
set_property -dict {PACKAGE_PIN AA20 IOSTANDARD LVCMOS33} [get_ports eth_rx_ctl]
set_property -dict {PACKAGE_PIN AA21 IOSTANDARD LVCMOS33} [get_ports {eth_rxd[0]}]
set_property -dict {PACKAGE_PIN V20  IOSTANDARD LVCMOS33} [get_ports {eth_rxd[1]}]
set_property -dict {PACKAGE_PIN U22  IOSTANDARD LVCMOS33} [get_ports {eth_rxd[2]}]
set_property -dict {PACKAGE_PIN V22  IOSTANDARD LVCMOS33} [get_ports {eth_rxd[3]}]
set_property -dict {PACKAGE_PIN V18 IOSTANDARD LVCMOS33 SLEW FAST DRIVE 8} [get_ports eth_txc]
set_property -dict {PACKAGE_PIN V19 IOSTANDARD LVCMOS33 SLEW FAST DRIVE 8} [get_ports eth_tx_ctl]
set_property -dict {PACKAGE_PIN T21 IOSTANDARD LVCMOS33 SLEW FAST DRIVE 8} [get_ports {eth_txd[0]}]
set_property -dict {PACKAGE_PIN U21 IOSTANDARD LVCMOS33 SLEW FAST DRIVE 8} [get_ports {eth_txd[1]}]
set_property -dict {PACKAGE_PIN P19 IOSTANDARD LVCMOS33 SLEW FAST DRIVE 8} [get_ports {eth_txd[2]}]
set_property -dict {PACKAGE_PIN R19 IOSTANDARD LVCMOS33 SLEW FAST DRIVE 8} [get_ports {eth_txd[3]}]
set_property -dict {PACKAGE_PIN N20 IOSTANDARD LVCMOS33} [get_ports eth_rst_n]

## =====================================================================
## 时序
## =====================================================================
create_clock -period 8.000 -name eth_rxc [get_ports eth_rxc]

## 顶层是 Block Design：模块内的路径为 mca_block_i/<方块名>/inst/...

set clk_eth   [get_clocks -of_objects [get_pins mca_block_i/u_clk/inst/u_mmcm/CLKOUT0]]
set clk_eth90 [get_clocks -of_objects [get_pins mca_block_i/u_clk/inst/u_mmcm/CLKOUT1]]
set clk_ref   [get_clocks -of_objects [get_pins mca_block_i/u_clk/inst/u_mmcm/CLKOUT2]]
set clk_adc   [get_clocks -of_objects [get_pins mca_block_i/u_clk/inst/u_pll/CLKOUT0]]
set clk_fwd   [get_clocks -of_objects [get_pins mca_block_i/u_clk/inst/u_pll/CLKOUT1]]

## 三个时钟域之间只经过异步 FIFO、两级同步器和握手（数据在握手期间保持不变）
set_clock_groups -asynchronous \
    -group [list $clk_eth $clk_eth90 $clk_ref] \
    -group [list $clk_adc $clk_fwd] \
    -group [get_clocks eth_rxc]

## ---------------- RGMII RX：PHY 内部加 RXC 延时（中心对齐 DDR） ----------------
## RGMII-ID 规范：数据在时钟沿前后各至少 1.2 ns 有效
set rx_dv 1.2
set_input_delay -clock eth_rxc -max [expr 4.0 - $rx_dv] [get_ports {eth_rxd[*] eth_rx_ctl}]
set_input_delay -clock eth_rxc -min $rx_dv              [get_ports {eth_rxd[*] eth_rx_ctl}]
set_input_delay -clock eth_rxc -max [expr 4.0 - $rx_dv] [get_ports {eth_rxd[*] eth_rx_ctl}] -clock_fall -add_delay
set_input_delay -clock eth_rxc -min $rx_dv              [get_ports {eth_rxd[*] eth_rx_ctl}] -clock_fall -add_delay

## ---------------- RGMII TX：TXC 由 90° 时钟的 ODDR 转发 ----------------
create_generated_clock -name eth_txc_out -source [get_pins mca_block_i/u_rgmii/inst/u_oddr_txc/C] -multiply_by 1 [get_ports eth_txc]
## PHY 侧（未开 TX 内部延时）要求建立 1.0 ns、保持 0.8 ns
set_output_delay -clock eth_txc_out -max  1.0 [get_ports {eth_txd[*] eth_tx_ctl}]
set_output_delay -clock eth_txc_out -min -0.8 [get_ports {eth_txd[*] eth_tx_ctl}]
set_output_delay -clock eth_txc_out -max  1.0 [get_ports {eth_txd[*] eth_tx_ctl}] -clock_fall -add_delay
set_output_delay -clock eth_txc_out -min -0.8 [get_ports {eth_txd[*] eth_tx_ctl}] -clock_fall -add_delay
## 数据在 clk_eth 两沿发出，TXC 晚 2 ns：默认的"最近沿"建立关系（2 ns）和
## 相邻半周期保持关系（-2 ns）正好是要检查的，其余组合都更宽松，不需要额外例外。

## ---------------- AD9280：ad_clk 由 ODDR 转发 ----------------
## 假设输出延时 tOD = 20..30 ns（相对 ad_clk 上升沿），表达为相对下一个沿 -11.25..-1.25 ns
create_generated_clock -name ad_clk_out -source [get_pins mca_block_i/u_adc/inst/u_oddr_adclk/C] -multiply_by 1 [get_ports ad_clk]
set_input_delay -clock ad_clk_out -max -1.25  [get_ports {ad_data[*] ad_otr}]
set_input_delay -clock ad_clk_out -min -11.25 [get_ports {ad_data[*] ad_otr}]

## ---------------- 慢速 IO ----------------
set_false_path -from [get_ports sys_rst_n]
set_false_path -to [get_ports {led[*] eth_rst_n}]

## ---------------- 配置 ----------------
set_property CFGBVS VCCO [current_design]
set_property CONFIG_VOLTAGE 3.3 [current_design]
set_property BITSTREAM.GENERAL.COMPRESS TRUE [current_design]
set_property BITSTREAM.CONFIG.SPI_BUSWIDTH 4 [current_design]
set_property CONFIG_MODE SPIx4 [current_design]
