`timescale 1ns / 1ps
// 以太网发送引擎（clk_eth 域）
//  1. COPY：按 pl_addr 读出载荷（源端 1 拍延迟），写入帧缓冲 [42..]，同时累加 UDP 校验和
//  2. SUM ：算 IP 头 / UDP 校验和（两拍）
//  3. HDR ：写以太网 + IPv4 + UDP 头（或 ARP 应答）到 [0..41]
//  4. SEND：前导 7×55 + D5，帧（补零到 60 字节），FCS，12 字节帧间隔
module eth_tx_engine (
    input             clk,
    input             rst,
    input      [47:0] local_mac,
    input      [31:0] local_ip,

    input             start,
    input             is_arp,
    input      [47:0] dst_mac,
    input      [31:0] dst_ip,
    input      [15:0] src_port,
    input      [15:0] dst_port,
    input      [9:0]  pl_len,        // UDP 载荷字节数（≤ 982）

    output     [9:0]  pl_addr,
    input      [7:0]  pl_data,
    output            busy,
    output reg        copy_done = 1'b0,

    input             byte_en,
    output reg        tx_en = 1'b0,
    output reg [7:0]  tx_data = 8'h00
);
    function [31:0] crc32_d8;
        input [31:0] c;
        input [7:0]  d;
        integer i;
        reg [31:0] x;
        begin
            x = c;
            for (i = 0; i < 8; i = i + 1)
                x = (x >> 1) ^ ((x[0] ^ d[i]) ? 32'hEDB88320 : 32'h0);
            crc32_d8 = x;
        end
    endfunction

    function [15:0] fold;
        input [31:0] s;
        reg   [31:0] t;
        begin
            t = {16'd0, s[15:0]} + {16'd0, s[31:16]};
            t = {16'd0, t[15:0]} + {16'd0, t[31:16]};
            fold = t[15:0];
        end
    endfunction

    localparam S_IDLE = 3'd0, S_COPY = 3'd1, S_SUM = 3'd2, S_SUM2 = 3'd3, S_HDR = 3'd4, S_SEND = 3'd5;
    reg [2:0] st = S_IDLE;
    assign busy = (st != S_IDLE);

    // 帧缓冲：分布式 RAM，同步写、异步读
    (* ram_style = "distributed" *) reg [7:0] buf_m [0:1023];
    reg        we = 1'b0;
    reg [9:0]  wa = 0;
    reg [7:0]  wd = 0;
    always @(posedge clk) if (we) buf_m[wa] <= wd;

    // 锁存的描述符
    reg        r_arp = 0;
    reg [47:0] r_mac = 0;
    reg [31:0] r_ip = 0;
    reg [15:0] r_sp = 0, r_dp = 0;
    reg [9:0]  r_len = 0;
    reg [15:0] ip_id = 0;

    // COPY
    reg [9:0]  ci = 0, ci_d = 0;
    reg        cv_d = 1'b0;
    reg [7:0]  hi_b = 0;
    reg [31:0] psum = 0;
    assign pl_addr = ci;

    // 校验和
    wire [15:0] ip_tot  = 16'd28 + {6'd0, r_len};
    wire [15:0] udp_len = 16'd8  + {6'd0, r_len};
    reg  [15:0] ip_csum = 0, udp_csum = 0;
    reg  [31:0] ip_sum = 0, udp_sum = 0;
    wire [31:0] addr_sum = local_ip[31:16] + local_ip[15:0] + r_ip[31:16] + r_ip[15:0];

    // HDR
    reg [5:0] hj = 0;
    reg [7:0] hb;
    always @(*) begin
        hb = 8'h00;
        case (hj)
            0:  hb = r_mac[47:40];  1: hb = r_mac[39:32];  2: hb = r_mac[31:24];
            3:  hb = r_mac[23:16];  4: hb = r_mac[15:8];   5: hb = r_mac[7:0];
            6:  hb = local_mac[47:40]; 7: hb = local_mac[39:32]; 8: hb = local_mac[31:24];
            9:  hb = local_mac[23:16]; 10: hb = local_mac[15:8]; 11: hb = local_mac[7:0];
            12: hb = 8'h08;
            13: hb = r_arp ? 8'h06 : 8'h00;
            default: ;
        endcase
        if (r_arp) begin
            case (hj)
                14: hb = 8'h00; 15: hb = 8'h01; 16: hb = 8'h08; 17: hb = 8'h00;
                18: hb = 8'h06; 19: hb = 8'h04; 20: hb = 8'h00; 21: hb = 8'h02;
                22: hb = local_mac[47:40]; 23: hb = local_mac[39:32]; 24: hb = local_mac[31:24];
                25: hb = local_mac[23:16]; 26: hb = local_mac[15:8];  27: hb = local_mac[7:0];
                28: hb = local_ip[31:24];  29: hb = local_ip[23:16];  30: hb = local_ip[15:8];
                31: hb = local_ip[7:0];
                32: hb = r_mac[47:40]; 33: hb = r_mac[39:32]; 34: hb = r_mac[31:24];
                35: hb = r_mac[23:16]; 36: hb = r_mac[15:8];  37: hb = r_mac[7:0];
                38: hb = r_ip[31:24];  39: hb = r_ip[23:16];  40: hb = r_ip[15:8];
                41: hb = r_ip[7:0];
                default: ;
            endcase
        end else begin
            case (hj)
                14: hb = 8'h45; 15: hb = 8'h00;
                16: hb = ip_tot[15:8];  17: hb = ip_tot[7:0];
                18: hb = ip_id[15:8];   19: hb = ip_id[7:0];
                20: hb = 8'h40; 21: hb = 8'h00;              // DF
                22: hb = 8'd64; 23: hb = 8'h11;              // TTL / UDP
                24: hb = ip_csum[15:8]; 25: hb = ip_csum[7:0];
                26: hb = local_ip[31:24]; 27: hb = local_ip[23:16];
                28: hb = local_ip[15:8];  29: hb = local_ip[7:0];
                30: hb = r_ip[31:24]; 31: hb = r_ip[23:16]; 32: hb = r_ip[15:8]; 33: hb = r_ip[7:0];
                34: hb = r_sp[15:8];  35: hb = r_sp[7:0];
                36: hb = r_dp[15:8];  37: hb = r_dp[7:0];
                38: hb = udp_len[15:8]; 39: hb = udp_len[7:0];
                40: hb = udp_csum[15:8]; 41: hb = udp_csum[7:0];
                default: ;
            endcase
        end
    end

    // SEND
    reg [10:0] q = 0;               // 当前字节序号（含前导）
    reg [1:0]  kind = 0;            // 0 前导，1 帧，2 FCS，3 帧间隔
    reg [31:0] crc = 32'hFFFF_FFFF;
    wire [10:0] dlen = 11'd42 + {1'b0, r_len};              // 有效帧字节
    wire [10:0] flen = (dlen < 11'd60) ? 11'd60 : dlen;     // 补零后
    wire [10:0] qn   = q + 1'b1;
    wire [10:0] fi   = qn - 11'd8;                           // 下一个帧字节下标
    wire [31:0] crc_n = crc32_d8(crc, tx_data);

    always @(posedge clk) begin
        we        <= 1'b0;
        copy_done <= 1'b0;
        if (rst) begin
            st <= S_IDLE; tx_en <= 1'b0; tx_data <= 8'h00;
        end else case (st)
            S_IDLE: begin
                tx_en <= 1'b0;
                if (start) begin
                    r_arp <= is_arp; r_mac <= dst_mac; r_ip <= dst_ip;
                    r_sp <= src_port; r_dp <= dst_port; r_len <= is_arp ? 10'd0 : pl_len;
                    ci <= 0; cv_d <= 1'b0; psum <= 0;
                    st <= is_arp ? S_SUM : S_COPY;
                end
            end
            S_COPY: begin
                // 发地址
                cv_d <= (ci < r_len);
                ci_d <= ci;
                if (ci < r_len) ci <= ci + 1'b1;
                // 收数据（上一拍地址）
                if (cv_d) begin
                    we <= 1'b1; wa <= 10'd42 + ci_d; wd <= pl_data;
                    if (!ci_d[0]) hi_b <= pl_data;
                    else          psum <= psum + {hi_b, pl_data};
                end
                if (!cv_d && ci == r_len && ci != 0) begin
                    if (r_len[0]) psum <= psum + {hi_b, 8'h00};
                    copy_done <= 1'b1;
                    st <= S_SUM;
                end else if (r_len == 0) begin
                    copy_done <= 1'b1;
                    st <= S_SUM;
                end
            end
            S_SUM: begin
                ip_sum  <= 32'h4500 + ip_tot + ip_id + 32'h4000 + 32'h4011 + addr_sum;
                udp_sum <= psum + addr_sum + 32'h0011 + udp_len + r_sp + r_dp + udp_len;
                if (r_arp) copy_done <= 1'b1;
                st <= S_SUM2;
            end
            S_SUM2: begin
                ip_csum  <= ~fold(ip_sum);
                udp_csum <= (fold(udp_sum) == 16'hFFFF) ? 16'hFFFF : ~fold(udp_sum);
                hj <= 0;
                st <= S_HDR;
            end
            S_HDR: begin
                we <= 1'b1; wa <= {4'd0, hj}; wd <= hb;
                hj <= hj + 1'b1;
                if (hj == 6'd41) begin
                    st <= S_SEND;
                    q <= 0; kind <= 2'd0;
                    tx_en <= 1'b1; tx_data <= 8'h55;
                    crc <= 32'hFFFF_FFFF;
                    ip_id <= ip_id + 1'b1;
                end
            end
            S_SEND: if (byte_en) begin
                q <= qn;
                if (kind == 2'd1) crc <= crc_n;
                if (qn < 11'd7) begin
                    tx_data <= 8'h55;
                end else if (qn == 11'd7) begin
                    tx_data <= 8'hD5;
                end else if (fi < flen) begin
                    kind    <= 2'd1;
                    tx_data <= (fi < dlen) ? buf_m[fi[9:0]] : 8'h00;
                end else if (fi < flen + 11'd4) begin
                    kind <= 2'd2;
                    case (fi - flen)
                        11'd0:   tx_data <= ~crc_n[7:0];
                        11'd1:   tx_data <= ~crc[15:8];
                        11'd2:   tx_data <= ~crc[23:16];
                        default: tx_data <= ~crc[31:24];
                    endcase
                end else if (fi < flen + 11'd16) begin
                    kind  <= 2'd3;
                    tx_en <= 1'b0;
                    tx_data <= 8'h00;
                end else begin
                    st <= S_IDLE;
                end
            end
            default: st <= S_IDLE;
        endcase
    end
endmodule
