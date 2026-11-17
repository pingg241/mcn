`timescale 1ns / 1ps
// 时钟生成：板载 50 MHz →
//   MMCM: clk_eth 125 MHz、clk_eth90 125 MHz/90°（RGMII TXC 相移）、clk_ref 200 MHz（IDELAYCTRL）
//   PLL : clk_adc 32 MHz（采样/DSP 时钟）、clk_adc_fwd 32 MHz/可调相位（送 AD9280 的 ad_clk）
module clk_gen #(
    parameter real ADC_CLK_PHASE = 180.0   // ad_clk 相对内部采样沿的相位，步进 1.8°
)(
    input  sys_clk,
    input  sys_rst_n,
    output clk_eth,
    output clk_eth90,
    output clk_ref,
    output clk_adc,
    output clk_adc_fwd,
    output locked
);
    wire clk_in_ibuf, clk_in;
    IBUF u_ibuf (.I(sys_clk), .O(clk_in_ibuf));
    BUFG u_bufg_in (.I(clk_in_ibuf), .O(clk_in));

    // ---------------- MMCM：VCO = 50 × 20 = 1000 MHz ----------------
    wire mm_fb, mm_fb_bufg, mm_o0, mm_o1, mm_o2, mm_locked;
    MMCME2_BASE #(
        .BANDWIDTH         ("OPTIMIZED"),
        .CLKIN1_PERIOD     (20.0),
        .DIVCLK_DIVIDE     (1),
        .CLKFBOUT_MULT_F   (20.0),
        .CLKOUT0_DIVIDE_F  (8.0),
        .CLKOUT0_PHASE     (0.0),
        .CLKOUT1_DIVIDE    (8),
        .CLKOUT1_PHASE     (90.0),
        .CLKOUT2_DIVIDE    (5),
        .CLKOUT2_PHASE     (0.0),
        .STARTUP_WAIT      ("FALSE")
    ) u_mmcm (
        .CLKIN1   (clk_in),
        .CLKFBIN  (mm_fb_bufg),
        .CLKFBOUT (mm_fb),
        .CLKFBOUTB(),
        .CLKOUT0  (mm_o0), .CLKOUT0B(),
        .CLKOUT1  (mm_o1), .CLKOUT1B(),
        .CLKOUT2  (mm_o2), .CLKOUT2B(),
        .CLKOUT3  (), .CLKOUT3B(),
        .CLKOUT4  (), .CLKOUT5(), .CLKOUT6(),
        .LOCKED   (mm_locked),
        .PWRDWN   (1'b0),
        .RST      (~sys_rst_n)
    );
    BUFG u_bufg_mfb (.I(mm_fb), .O(mm_fb_bufg));
    BUFG u_bufg_eth (.I(mm_o0), .O(clk_eth));
    BUFG u_bufg_e90 (.I(mm_o1), .O(clk_eth90));
    BUFG u_bufg_ref (.I(mm_o2), .O(clk_ref));

    // ---------------- PLL：VCO = 50 × 16 = 800 MHz ----------------
    wire pl_fb, pl_o0, pl_o1, pl_locked;
    PLLE2_BASE #(
        .BANDWIDTH        ("OPTIMIZED"),
        .CLKIN1_PERIOD    (20.0),
        .DIVCLK_DIVIDE    (1),
        .CLKFBOUT_MULT    (16),
        .CLKOUT0_DIVIDE   (25),
        .CLKOUT0_PHASE    (0.0),
        .CLKOUT1_DIVIDE   (25),
        .CLKOUT1_PHASE    (ADC_CLK_PHASE),
        .STARTUP_WAIT     ("FALSE")
    ) u_pll (
        .CLKIN1   (clk_in),
        .CLKFBIN  (pl_fb),
        .CLKFBOUT (pl_fb),
        .CLKOUT0  (pl_o0),
        .CLKOUT1  (pl_o1),
        .CLKOUT2  (), .CLKOUT3(), .CLKOUT4(), .CLKOUT5(),
        .LOCKED   (pl_locked),
        .PWRDWN   (1'b0),
        .RST      (~sys_rst_n)
    );
    BUFG u_bufg_adc (.I(pl_o0), .O(clk_adc));
    BUFG u_bufg_fwd (.I(pl_o1), .O(clk_adc_fwd));

    assign locked = mm_locked & pl_locked;
endmodule
