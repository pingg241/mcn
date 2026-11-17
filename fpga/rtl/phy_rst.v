`timescale 1ns / 1ps
// PHY 硬件复位：系统复位释放后再保持 CYCLES 拍才放开 phy_rst_n（默认 125 MHz 下 20 ms）
module phy_rst #(
    parameter CYCLES = 2_500_000
)(
    input      clk,
    input      rst,
    output reg phy_rst_n = 1'b0
);
    reg [$clog2(CYCLES + 1)-1:0] cnt = 0;
    always @(posedge clk) begin
        if (rst) begin
            cnt <= 0; phy_rst_n <= 1'b0;
        end else if (cnt < CYCLES) begin
            cnt <= cnt + 1'b1; phy_rst_n <= 1'b0;
        end else begin
            phy_rst_n <= 1'b1;
        end
    end
endmodule
