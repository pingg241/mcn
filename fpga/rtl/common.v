`timescale 1ns / 1ps
// 通用小模块：同步器、异步 FIFO、可变延时线

// 多位电平同步器（只用于准静态信号或单比特信号）
module sync_ff #(
    parameter W    = 1,
    parameter INIT = 0
)(
    input              clk,
    input  [W-1:0]     d,
    output [W-1:0]     q
);
    (* ASYNC_REG = "TRUE" *) reg [W-1:0] s1 = INIT;
    (* ASYNC_REG = "TRUE" *) reg [W-1:0] s2 = INIT;
    always @(posedge clk) begin
        s1 <= d;
        s2 <= s1;
    end
    assign q = s2;
endmodule

// 复位同步：异步置位、同步释放，高有效输出
module rst_sync (
    input  clk,
    input  arst,        // 高有效异步复位
    output rst
);
    (* ASYNC_REG = "TRUE" *) reg [3:0] sr = 4'hF;
    always @(posedge clk or posedge arst) begin
        if (arst) sr <= 4'hF;
        else      sr <= {sr[2:0], 1'b0};
    end
    assign rst = sr[3];
endmodule

// 异步 FIFO：格雷码指针，读出有 1 拍延迟（rd_en 后下一拍 dout 有效）
module async_fifo #(
    parameter DW = 8,
    parameter AW = 6
)(
    input               wr_clk,
    input               wr_rst,
    input               wr_en,
    input  [DW-1:0]     din,
    output              full,

    input               rd_clk,
    input               rd_rst,
    input               rd_en,
    output reg [DW-1:0] dout,
    output              empty
);
    reg [DW-1:0] mem [0:(1<<AW)-1];

    reg [AW:0] wbin = 0, wgray = 0;
    reg [AW:0] rbin = 0, rgray = 0;
    (* ASYNC_REG = "TRUE" *) reg [AW:0] rg_w1 = 0, rg_w2 = 0;   // 读指针 → 写时钟域
    (* ASYNC_REG = "TRUE" *) reg [AW:0] wg_r1 = 0, wg_r2 = 0;   // 写指针 → 读时钟域

    wire [AW:0] wbin_n  = wbin + 1'b1;
    wire [AW:0] wgray_n = wbin_n ^ (wbin_n >> 1);
    assign full = (wgray == {~rg_w2[AW:AW-1], rg_w2[AW-2:0]});

    always @(posedge wr_clk) begin
        if (wr_rst) begin
            wbin <= 0; wgray <= 0;
        end else if (wr_en && !full) begin
            mem[wbin[AW-1:0]] <= din;
            wbin  <= wbin_n;
            wgray <= wgray_n;
        end
    end
    always @(posedge wr_clk) begin
        rg_w1 <= rgray; rg_w2 <= rg_w1;
    end

    wire [AW:0] rbin_n  = rbin + 1'b1;
    wire [AW:0] rgray_n = rbin_n ^ (rbin_n >> 1);
    assign empty = (rgray == wg_r2);

    always @(posedge rd_clk) begin
        if (rd_rst) begin
            rbin <= 0; rgray <= 0;
        end else if (rd_en && !empty) begin
            dout  <= mem[rbin[AW-1:0]];
            rbin  <= rbin_n;
            rgray <= rgray_n;
        end
    end
    always @(posedge rd_clk) begin
        wg_r1 <= wgray; wg_r2 <= wg_r1;
    end
endmodule

// 可变延时线：环形缓冲。每拍写入 din，
// 输出 now = din(n)，del = din(n-dly)，两者在同一拍对齐（各晚 1 拍）。
// dly 取值 1 .. 2^AW-1。
module delay_line #(
    parameter W  = 8,
    parameter AW = 10
)(
    input                  clk,
    input  [W-1:0]         din,
    input  [AW-1:0]        dly,
    output reg [W-1:0]     now,
    output reg [W-1:0]     del
);
    reg [W-1:0]  mem [0:(1<<AW)-1];
    reg [AW-1:0] wp = 0;
    integer i;
    initial for (i = 0; i < (1<<AW); i = i + 1) mem[i] = 0;

    always @(posedge clk) begin
        mem[wp] <= din;
        del     <= mem[wp - dly];
        now     <= din;
        wp      <= wp + 1'b1;
    end
endmodule
