`timescale 1ns / 1ps
// 以太网接收解析（clk_eth 域）：从 RX FIFO 取 {last, err, data}，
// 识别发给本机的 ARP 请求和 UDP 5000 端口的 MCCM 命令，帧尾 CRC 正确才输出。
module eth_rx_parser (
    input             clk,
    input             rst,
    input      [47:0] local_mac,
    input      [31:0] local_ip,
    input      [15:0] cmd_port,

    output            rd_en,
    input      [9:0]  rd_data,
    input             empty,

    output reg        arp_valid = 1'b0,
    output reg [47:0] arp_sha = 0,
    output reg [31:0] arp_spa = 0,

    output reg        cmd_valid = 1'b0,
    output reg [47:0] cmd_src_mac = 0,
    output reg [31:0] cmd_src_ip = 0,
    output reg [15:0] cmd_src_port = 0,
    output reg [7:0]  cmd_code = 0,
    output reg [7:0]  cmd_seq = 0,
    output reg [31:0] cmd_addr = 0,
    output reg [31:0] cmd_value = 0,

    output reg [31:0] rx_frames = 0,     // CRC 正确的帧
    output reg [31:0] rx_errors = 0      // CRC/PHY 错误帧
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

    assign rd_en = !empty;
    reg v = 1'b0;
    always @(posedge clk) v <= rd_en && !rst;

    wire       last = rd_data[9];
    wire       err  = rd_data[8];
    wire [7:0] b    = rd_data[7:0];

    reg [10:0] idx = 0;
    reg [31:0] crc = 32'hFFFF_FFFF;
    reg        m_local = 1, m_bcast = 1;     // 目的 MAC 匹配
    reg [15:0] etype = 0;
    reg        ok_arp = 1, ok_ip = 1;        // 逐字段检查
    reg        ip_local = 1, ip_bcast = 1;

    reg [47:0] f_src_mac = 0;
    reg [47:0] f_sha = 0;
    reg [31:0] f_spa = 0, f_sip = 0, f_addr = 0, f_val = 0;
    reg [15:0] f_sport = 0;
    reg [7:0]  f_cmd = 0, f_seq = 0;

    // 本拍字节对应的期望值
    reg [7:0] mac_b;                                   // idx 0..5
    always @(*) begin
        case (idx[2:0])
            3'd0: mac_b = local_mac[47:40];
            3'd1: mac_b = local_mac[39:32];
            3'd2: mac_b = local_mac[31:24];
            3'd3: mac_b = local_mac[23:16];
            3'd4: mac_b = local_mac[15:8];
            default: mac_b = local_mac[7:0];
        endcase
    end
    // ARP 目标 IP（38..41）与 IP 目的地址（30..33）的低 2 位都从 2 开始
    wire [1:0] ip_sel = idx[1:0] - 2'd2;
    wire [7:0] tpa_b  = local_ip[8*(3 - ip_sel) +: 8];
    wire [7:0] dip_b  = tpa_b;

    wire [31:0] crc_n = crc32_d8(crc, b);

    always @(posedge clk) begin
        arp_valid <= 1'b0;
        cmd_valid <= 1'b0;
        if (rst) begin
            idx <= 0; crc <= 32'hFFFF_FFFF;
            m_local <= 1; m_bcast <= 1; ok_arp <= 1; ok_ip <= 1; ip_local <= 1; ip_bcast <= 1;
        end else if (v) begin
            crc <= crc_n;
            idx <= (idx == 11'h7FF) ? idx : idx + 1'b1;

            // 目的 MAC
            if (idx < 6) begin
                if (b != mac_b)  m_local <= 1'b0;
                if (b != 8'hFF)  m_bcast <= 1'b0;
            end
            if (idx >= 6 && idx < 12) f_src_mac <= {f_src_mac[39:0], b};
            if (idx == 12) etype[15:8] <= b;
            if (idx == 13) etype[7:0]  <= b;

            // ARP（以太网类型 0x0806）
            case (idx)
                14: if (b != 8'h00) ok_arp <= 1'b0;
                15: if (b != 8'h01) ok_arp <= 1'b0;
                16: if (b != 8'h08) ok_arp <= 1'b0;
                17: if (b != 8'h00) ok_arp <= 1'b0;
                18: if (b != 8'h06) ok_arp <= 1'b0;
                19: if (b != 8'h04) ok_arp <= 1'b0;
                20: if (b != 8'h00) ok_arp <= 1'b0;
                21: if (b != 8'h01) ok_arp <= 1'b0;   // 仅处理请求
                default: ;
            endcase
            if (idx >= 22 && idx < 28) f_sha <= {f_sha[39:0], b};
            if (idx >= 28 && idx < 32) f_spa <= {f_spa[23:0], b};
            if (idx >= 38 && idx < 42 && b != tpa_b) ok_arp <= 1'b0;

            // IPv4 / UDP / MCCM（以太网类型 0x0800）
            case (idx)
                14: if (b != 8'h45) ok_ip <= 1'b0;                 // 无选项
                20: if ((b & 8'h3F) != 8'h00) ok_ip <= 1'b0;       // 不分片
                21: if (b != 8'h00) ok_ip <= 1'b0;
                23: if (b != 8'h11) ok_ip <= 1'b0;                 // UDP
                36: if (b != cmd_port[15:8]) ok_ip <= 1'b0;
                37: if (b != cmd_port[7:0])  ok_ip <= 1'b0;
                42: if (b != "M") ok_ip <= 1'b0;
                43: if (b != "C") ok_ip <= 1'b0;
                44: if (b != "C") ok_ip <= 1'b0;
                45: if (b != "M") ok_ip <= 1'b0;
                46: f_cmd <= b;
                47: f_seq <= b;
                default: ;
            endcase
            if (idx >= 26 && idx < 30) f_sip <= {f_sip[23:0], b};
            if (idx >= 30 && idx < 34) begin
                if (b != dip_b) ip_local <= 1'b0;
                if (b != 8'hFF) ip_bcast <= 1'b0;
            end
            if (idx >= 34 && idx < 36) f_sport <= {f_sport[7:0], b};
            if (idx >= 50 && idx < 54) f_addr  <= {f_addr[23:0], b};
            if (idx >= 54 && idx < 58) f_val   <= {f_val[23:0], b};

            if (last) begin
                // CRC 残余：包含 FCS 的整帧 CRC 寄存器应为 0xDEBB20E3
                if (err || crc_n != 32'hDEBB20E3) begin
                    rx_errors <= rx_errors + 1'b1;
                end else begin
                    rx_frames <= rx_frames + 1'b1;
                    if ((m_local || m_bcast) && etype == 16'h0806 && ok_arp && idx >= 45) begin
                        arp_valid <= 1'b1;
                        arp_sha   <= f_sha;
                        arp_spa   <= f_spa;
                    end
                    if ((m_local || m_bcast) && etype == 16'h0800 && ok_ip && (ip_local || ip_bcast) && idx >= 61) begin
                        cmd_valid    <= 1'b1;
                        cmd_src_mac  <= f_src_mac;
                        cmd_src_ip   <= f_sip;
                        cmd_src_port <= f_sport;
                        cmd_code     <= f_cmd;
                        cmd_seq      <= f_seq;
                        cmd_addr     <= f_addr;
                        cmd_value    <= f_val;
                    end
                end
                idx <= 0; crc <= 32'hFFFF_FFFF;
                m_local <= 1; m_bcast <= 1; ok_arp <= 1; ok_ip <= 1; ip_local <= 1; ip_bcast <= 1;
            end
        end
    end
endmodule
