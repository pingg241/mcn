`timescale 1ns / 1ps
// AD9280 接口：ODDR 转发 ad_clk，IOB 寄存器同步捕获 8 位并行数据和 OTR
module adc_if (
    input            clk_adc,      // 内部采样时钟
    input            clk_adc_fwd,  // 带相位的转发时钟
    (* X_INTERFACE_PARAMETER = "FREQ_HZ 32000000" *)   // Block Design 里输出时钟要标频率
    output           ad_clk,
    input  [7:0]     ad_data,
    input            ad_otr,
    output reg [7:0] sample,
    output reg       otr
);
    ODDR #(
        .DDR_CLK_EDGE("SAME_EDGE"),
        .INIT(1'b0),
        .SRTYPE("SYNC")
    ) u_oddr_adclk (
        .Q(ad_clk), .C(clk_adc_fwd), .CE(1'b1),
        .D1(1'b1), .D2(1'b0), .R(1'b0), .S(1'b0)
    );

    (* IOB = "TRUE" *) reg [7:0] d_iob;
    (* IOB = "TRUE" *) reg       otr_iob;
    always @(posedge clk_adc) begin
        d_iob   <= ad_data;
        otr_iob <= ad_otr;
        sample  <= d_iob;
        otr     <= otr_iob;
    end
endmodule
