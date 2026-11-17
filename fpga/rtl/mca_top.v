`timescale 1ns / 1ps
// 核脉冲多道分析器 FPGA 前端顶层（正点原子达芬奇Pro + ATK-HS-AD/DA 模块）
module mca_top #(
    parameter real ADC_CLK_PHASE  = 180.0,      // ad_clk 相位（度，步进 1.8）
    parameter      TXC_90         = 1,          // RGMII TXC 90° 相移
    parameter      IDELAY_VALUE   = 10,         // RX 数据延时抽头（78 ps/抽头），按 RGMII-ID 最坏窗口居中
    parameter      PHY_RST_CYCLES = 2_500_000,  // PHY 复位保持 20 ms
    parameter      HB_CYCLES      = 125_000_000,
    parameter      CYC_PER_MS     = 125_000
)(
    input        sys_clk,          // 50 MHz
    input        sys_rst_n,

    output       ad_clk,
    input  [7:0] ad_data,
    input        ad_otr,

    input        eth_rxc,
    input        eth_rx_ctl,
    input  [3:0] eth_rxd,
    output       eth_txc,
    output       eth_tx_ctl,
    output [3:0] eth_txd,
    output reg   eth_rst_n = 1'b0,

    output [3:0] led
);
    // ---------------- 时钟与复位 ----------------
    wire clk_eth, clk_eth90, clk_ref, clk_adc, clk_adc_fwd, locked;
    clk_gen #(.ADC_CLK_PHASE(ADC_CLK_PHASE)) u_clk (
        .sys_clk(sys_clk), .sys_rst_n(sys_rst_n),
        .clk_eth(clk_eth), .clk_eth90(clk_eth90), .clk_ref(clk_ref),
        .clk_adc(clk_adc), .clk_adc_fwd(clk_adc_fwd), .locked(locked)
    );
    wire arst = ~sys_rst_n | ~locked;
    wire rst_eth, rst_adc;
    rst_sync u_rs_eth (.clk(clk_eth), .arst(arst), .rst(rst_eth));
    rst_sync u_rs_adc (.clk(clk_adc), .arst(arst), .rst(rst_adc));

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

    // ---------------- ADC ----------------
    wire [7:0] adc_sample;
    wire       adc_otr_q;
    adc_if u_adc (
        .clk_adc(clk_adc), .clk_adc_fwd(clk_adc_fwd),
        .ad_clk(ad_clk), .ad_data(ad_data), .ad_otr(ad_otr),
        .sample(adc_sample), .otr(adc_otr_q)
    );

    // ---------------- 跨域信号 ----------------
    wire [32*24-1:0] cfg_bus;
    wire        op_tgl, op_ack_tgl;
    wire [2:0]  op_code;
    wire [31:0] op_arg;
    wire [63:0] snap_total_events, snap_total_ticks, snap_busy_ticks;
    wire [31:0] snap_pileup, snap_dropped, snap_sat;
    wire [15:0] snap_baseline;
    wire [47:0] snap_ts;
    wire [7:0]  snap_flags;
    wire        ev_valid, ev_full, ev_rd, ev_empty;
    wire [71:0] ev_data, ev_dout;
    wire        wave_done_tgl;
    wire [11:0] wave_start, wave_rd_addr;
    wire [12:0] wave_depth;
    wire [15:0] wave_pretrig, wave_rd_data;
    wire [47:0] wave_ts;
    wire [7:0]  wave_src;
    wire        led_event, led_host, led_run;

    adc_domain u_adcd (
        .clk(clk_adc), .rst(rst_adc),
        .adc_sample(adc_sample), .adc_otr(adc_otr_q),
        .cfg_bus(cfg_bus), .op_tgl(op_tgl), .op_code(op_code), .op_arg(op_arg),
        .op_ack_tgl(op_ack_tgl),
        .snap_total_events(snap_total_events), .snap_pileup(snap_pileup),
        .snap_dropped(snap_dropped), .snap_sat(snap_sat),
        .snap_total_ticks(snap_total_ticks), .snap_busy_ticks(snap_busy_ticks),
        .snap_baseline(snap_baseline), .snap_ts(snap_ts), .snap_flags(snap_flags),
        .ev_valid(ev_valid), .ev_data(ev_data), .ev_full(ev_full),
        .rd_clk(clk_eth), .wave_rd_addr(wave_rd_addr), .wave_rd_data(wave_rd_data),
        .wave_done_tgl(wave_done_tgl), .wave_start(wave_start), .wave_depth(wave_depth),
        .wave_pretrig(wave_pretrig), .wave_ts(wave_ts), .wave_src(wave_src),
        .led_event(led_event)
    );

    async_fifo #(.DW(72), .AW(10)) u_ev_fifo (
        .wr_clk(clk_adc), .wr_rst(rst_adc), .wr_en(ev_valid), .din(ev_data), .full(ev_full),
        .rd_clk(clk_eth), .rd_rst(rst_eth), .rd_en(ev_rd), .dout(ev_dout), .empty(ev_empty)
    );

    // ---------------- 以太网 ----------------
    wire        rx_rd_en, rx_empty, byte_en, tx_en;
    wire [9:0]  rx_dout;
    wire [7:0]  tx_data;
    wire [1:0]  speed;

    rgmii_if #(.IDELAY_VALUE(IDELAY_VALUE), .TXC_90(TXC_90)) u_rgmii (
        .clk_eth(clk_eth), .clk_eth90(clk_eth90), .clk_ref(clk_ref), .rst_eth(rst_eth),
        .rgmii_rxc(eth_rxc), .rgmii_rx_ctl(eth_rx_ctl), .rgmii_rxd(eth_rxd),
        .rgmii_txc(eth_txc), .rgmii_tx_ctl(eth_tx_ctl), .rgmii_txd(eth_txd),
        .rx_rd_en(rx_rd_en), .rx_dout(rx_dout), .rx_empty(rx_empty),
        .tx_en(tx_en), .tx_data(tx_data), .byte_en(byte_en),
        .speed(speed)
    );

    eth_core #(.HB_CYCLES(HB_CYCLES), .CYC_PER_MS(CYC_PER_MS)) u_core (
        .clk(clk_eth), .rst(rst_eth), .speed(speed),
        .rx_rd_en(rx_rd_en), .rx_dout(rx_dout), .rx_empty(rx_empty),
        .byte_en(byte_en), .tx_en(tx_en), .tx_data(tx_data),
        .cfg_bus(cfg_bus), .op_tgl(op_tgl), .op_code(op_code), .op_arg(op_arg),
        .op_ack_tgl(op_ack_tgl),
        .snap_total_events(snap_total_events), .snap_pileup(snap_pileup),
        .snap_dropped(snap_dropped), .snap_sat(snap_sat),
        .snap_total_ticks(snap_total_ticks), .snap_busy_ticks(snap_busy_ticks),
        .snap_baseline(snap_baseline), .snap_ts(snap_ts), .snap_flags(snap_flags),
        .ev_rd(ev_rd), .ev_dout(ev_dout), .ev_empty(ev_empty),
        .wave_done_tgl(wave_done_tgl), .wave_start(wave_start), .wave_depth(wave_depth),
        .wave_pretrig(wave_pretrig), .wave_ts(wave_ts), .wave_src(wave_src),
        .wave_rd_addr(wave_rd_addr), .wave_rd_data(wave_rd_data),
        .led_host(led_host), .led_run(led_run)
    );

    // ---------------- 指示灯 ----------------
    // LED0 心跳闪烁，LED1 链路（有 RXC），LED2 RUN，LED3 事件
    reg [26:0] blink = 0;
    always @(posedge clk_eth) blink <= blink + 1'b1;
    assign led = {led_event, led_run, (speed != 2'd0), blink[26] & eth_rst_n};
endmodule
