`timescale 1ns / 1ps
// RGMII PHY 行为模型（带 RX 时钟延时，即 RGMII-ID）
//  - send_frame(len)：把 fbuf[0..len-1]（含 FCS）加前导/SFD 送给 FPGA
//  - 接收 FPGA 发出的帧，去掉前导/SFD 后连同 FCS 写入文件
module phy_model #(
    parameter GIGA = 1                 // 1：1000M（125 MHz DDR），0：100M（25 MHz 半字节）
)(
    output           rxc,
    output reg       rx_ctl = 1'b0,
    output reg [3:0] rxd = 4'h0,
    input            txc,
    input            tx_ctl,
    input      [3:0] txd
);
    localparam real HALF = GIGA ? 4.0 : 20.0;
    reg clk_i = 1'b0;
    always #(HALF) clk_i = ~clk_i;
    assign #(GIGA ? 2.0 : 10.0) rxc = clk_i;

    reg [7:0] fbuf [0:2047];
    integer   sent = 0;

    task put_byte;
        input [7:0] b;
        begin
            if (GIGA) begin
                @(posedge clk_i); rxd <= b[3:0]; rx_ctl <= 1'b1;
                @(negedge clk_i); rxd <= b[7:4]; rx_ctl <= 1'b1;
            end else begin
                @(posedge clk_i); rxd <= b[3:0]; rx_ctl <= 1'b1;
                @(posedge clk_i); rxd <= b[7:4]; rx_ctl <= 1'b1;
            end
        end
    endtask

    task send_frame;
        input integer len;
        integer i;
        begin
            for (i = 0; i < 7; i = i + 1) put_byte(8'h55);
            put_byte(8'hD5);
            for (i = 0; i < len; i = i + 1) put_byte(fbuf[i]);
            @(posedge clk_i); rx_ctl <= 1'b0; rxd <= 4'h0;
            for (i = 0; i < 12 * (GIGA ? 1 : 2); i = i + 1) @(posedge clk_i);
            sent = sent + 1;
        end
    endtask

    // ---------------- 接收 FPGA 发送 ----------------
    integer fo;
    initial fo = $fopen("tx_frames.txt", "w");

    reg [7:0]  rbuf [0:2047];
    integer    rn = 0;
    reg        in_frame = 1'b0;
    reg [3:0]  lo_n;
    reg        en_r;
    reg        nib_ph = 1'b0;
    integer    got = 0;
    integer    k;

    task finish_frame;
        integer s, j;
        begin
            // 跳过前导 55..D5
            s = 0;
            while (s < rn && rbuf[s] == 8'h55) s = s + 1;
            if (s < rn && rbuf[s] == 8'hD5) begin
                $fwrite(fo, "%0d", $time);
                for (j = s + 1; j < rn; j = j + 1) $fwrite(fo, " %02x", rbuf[j]);
                $fwrite(fo, "\n");
                $fflush(fo);
                got = got + 1;
            end else begin
                $display("[%0t] PHY: 前导错误，%0d 字节", $time, rn);
            end
            rn = 0;
        end
    endtask

    generate
        if (GIGA) begin : g_rx1g
            always @(posedge txc) begin
                lo_n = txd;
                en_r = tx_ctl;
            end
            always @(negedge txc) begin
                if (en_r && tx_ctl) begin
                    rbuf[rn] = {txd, lo_n};
                    rn = rn + 1;
                    in_frame = 1'b1;
                end else if (in_frame) begin
                    in_frame = 1'b0;
                    finish_frame;
                end
            end
        end else begin : g_rx100
            always @(posedge txc) begin
                if (tx_ctl) begin
                    if (!nib_ph) lo_n = txd;
                    else begin rbuf[rn] = {txd, lo_n}; rn = rn + 1; end
                    nib_ph = ~nib_ph;
                    in_frame = 1'b1;
                end else if (in_frame) begin
                    in_frame = 1'b0;
                    nib_ph = 1'b0;
                    finish_frame;
                end
            end
        end
    endgenerate
endmodule
