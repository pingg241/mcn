`timescale 1ns / 1ps
// MCEV 事件包打包（clk_eth 域）
//  事件 FIFO 条目 {ts[47:0], energy[15:0], flags[7:0]}
//  满 n_max 个、首事件后超时、或时间戳高 16 位（epoch）变化时发出一包。
//  载荷：16 B 包头 + 8 B × n，经 pl_addr/pl_data（1 拍延迟）由发送引擎读取。
module mcev_packer (
    input             clk,
    input             rst,
    input             enable,          // 有主机时才取事件
    input      [6:0]  n_max,           // 1..64
    input      [31:0] timeout_cyc,     // 超时（clk_eth 拍）

    output            fifo_rd,
    input      [71:0] fifo_dout,
    input             fifo_empty,

    output reg        req = 1'b0,      // 请求发送
    input             done,            // 引擎已拷完载荷
    output     [9:0]  len,
    input      [9:0]  pl_addr,
    output reg [7:0]  pl_data = 0,
    output reg [31:0] seq = 0          // 下一包序号（= 已发包数）
);
    (* ram_style = "distributed" *) reg [55:0] ev_m [0:63];   // {ts_low, energy, flags}
    reg [6:0]  n = 0;
    reg [15:0] epoch = 0;
    reg [31:0] tmr = 0;

    reg        rd_pend = 1'b0;     // 已发读，下一拍数据有效
    reg        have = 1'b0;        // 手里有一个待放入的事件
    reg [71:0] ev = 0;

    assign len = 10'd16 + {n[6:0], 3'b000};

    wire can_rd = enable && !req && !have && !rd_pend && !fifo_empty;
    assign fifo_rd = can_rd;

    wire [15:0] ev_epoch = ev[71:56];
    wire flush_epoch = have && (n != 0) && (ev_epoch != epoch);

    always @(posedge clk) begin
        if (rst) begin
            n <= 0; req <= 1'b0; have <= 1'b0; rd_pend <= 1'b0; tmr <= 0; seq <= 0;
        end else begin
            rd_pend <= can_rd;
            if (rd_pend) begin
                ev   <= fifo_dout;
                have <= 1'b1;
            end

            if (req) begin
                if (done) begin
                    req <= 1'b0;
                    n   <= 0;
                    seq <= seq + 1'b1;
                end
            end else if (flush_epoch) begin
                req <= 1'b1;
            end else if (have) begin
                ev_m[n[5:0]] <= {ev[55:24], ev[23:8], ev[7:0]};
                if (n == 0) begin
                    epoch <= ev_epoch;
                    tmr   <= 0;
                end
                n    <= n + 1'b1;
                have <= 1'b0;
                if (n + 1'b1 >= n_max) req <= 1'b1;
            end else if (n != 0) begin
                tmr <= tmr + 1'b1;
                if (tmr >= timeout_cyc) req <= 1'b1;
            end
        end
    end

    // 载荷读口
    wire [9:0]  ea = pl_addr - 10'd16;
    wire [55:0] w  = ev_m[ea[8:3]];
    always @(posedge clk) begin
        if (pl_addr < 16) begin
            case (pl_addr[3:0])
                0: pl_data <= "M"; 1: pl_data <= "C"; 2: pl_data <= "E"; 3: pl_data <= "V";
                4: pl_data <= 8'h01;                    // version
                5: pl_data <= 8'h00;                    // flags
                6: pl_data <= 8'h00;
                7: pl_data <= {1'b0, n};                // n_events
                8: pl_data <= seq[31:24]; 9: pl_data <= seq[23:16];
                10: pl_data <= seq[15:8]; 11: pl_data <= seq[7:0];
                12: pl_data <= epoch[15:8]; 13: pl_data <= epoch[7:0];
                default: pl_data <= 8'h00;
            endcase
        end else begin
            case (ea[2:0])
                0: pl_data <= w[55:48]; 1: pl_data <= w[47:40];
                2: pl_data <= w[39:32]; 3: pl_data <= w[31:24];
                4: pl_data <= w[23:16]; 5: pl_data <= w[15:8];
                6: pl_data <= w[7:0];   default: pl_data <= 8'h00;
            endcase
        end
    end
endmodule
