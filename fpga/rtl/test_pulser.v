`timescale 1ns / 1ps
// 内置测试脉冲源：在 ADC 时钟域生成"像 AD9280 输出"的 8 位波形。
// 到达过程为每拍伯努利试验（近似泊松），脉冲为单极点指数衰减（b = 1 - 2^-decay，
// 对应 PZ_M = 2^decay - 1），可选一阶上升沿和均匀噪声。
module test_pulser (
    input             clk,
    input             rst,
    input      [31:0] rate,    // 每拍产生脉冲的概率 × 2^32，0 关闭
    input      [15:0] amp,     // 幅度，8.8 定点（单位 LSB）
    input      [1:0]  mode,    // 0 固定幅度；1 一半固定 + 一半 [0,amp) 均匀（峰 + 平台）；2 全部均匀
    input      [3:0]  noise,   // 均匀噪声 ±noise LSB
    input      [3:0]  decay,   // 衰减移位
    input      [3:0]  rise,    // 上升移位，0 为理想阶跃
    input      [7:0]  base,    // 基线
    input             neg,     // 1 时输出负极性脉冲（以 255 为镜像）
    output reg [7:0]  sample,
    output reg        fire
);
    reg [31:0] r1 = 32'h1234_5678, r2 = 32'h9E37_79B9, r3 = 32'h7F4A_7C15;
    function [31:0] xs32;
        input [31:0] x;
        reg   [31:0] t;
        begin
            t = x ^ (x << 13);
            t = t ^ (t >> 17);
            xs32 = t ^ (t << 5);
        end
    endfunction

    reg  signed [35:0] q = 0, y = 0;      // 16.16 定点
    wire [31:0] amp_frac = amp * r2[15:0];
    reg  [15:0] amp_sel;
    always @(*) begin
        case (mode)
            2'd1:    amp_sel = r2[31] ? amp : amp_frac[31:16];
            2'd2:    amp_sel = amp_frac[31:16];
            default: amp_sel = amp;
        endcase
    end

    wire signed [35:0] inj    = fire ? $signed({12'd0, amp_sel, 8'd0}) : 36'sd0;
    wire signed [35:0] q_next = q - (q >>> decay) + inj;
    wire signed [35:0] y_next = (rise == 4'd0) ? q_next : (y + ((q - y) >>> rise));

    wire signed [16:0] nz_u = $signed({1'b0, r3[15:0]}) - 17'sd32768;  // [-32768, 32767] ≈ ±0.5 LSB
    wire signed [22:0] nz   = nz_u * $signed({1'b0, noise, 1'b0});     // ±noise LSB（16 位小数）
    wire signed [35:0] tot  = $signed({12'd0, base, 16'd0}) + y + nz + 36'sd32768;
    wire signed [19:0] tot_i = tot[35:16];
    wire [7:0] clip = tot_i < 0 ? 8'd0 : (tot_i > 255 ? 8'd255 : tot_i[7:0]);

    always @(posedge clk) begin
        if (rst) begin
            q <= 0; y <= 0; fire <= 1'b0; sample <= base;
        end else begin
            r1 <= xs32(r1);
            r2 <= xs32(r2);
            r3 <= xs32(r3);
            fire   <= (rate != 32'd0) && (r1 < rate);
            q      <= q_next;
            y      <= y_next;
            sample <= neg ? ~clip : clip;
        end
    end
endmodule
