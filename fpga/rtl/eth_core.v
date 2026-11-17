`timescale 1ns / 1ps
// 以太网时钟域总控：寄存器、命令执行、主机学习、心跳、载荷生成、发送仲裁
module eth_core #(
    parameter [47:0] DEF_MAC    = 48'h02_00_4D_43_41_01,
    parameter [31:0] DEF_IP     = {8'd192, 8'd168, 8'd1, 8'd10},
    parameter        HB_CYCLES  = 125_000_000,   // 心跳周期（1 s）
    parameter        CYC_PER_MS = 125_000
)(
    input              clk,
    input              rst,
    input      [1:0]   speed,

    output             rx_rd_en,
    input      [9:0]   rx_dout,
    input              rx_empty,

    input              byte_en,
    output             tx_en,
    output     [7:0]   tx_data,

    // ADC 域
    output [32*24-1:0] cfg_bus,
    output reg         op_tgl = 1'b0,
    output reg [2:0]   op_code = 0,
    output reg [31:0]  op_arg = 0,
    input              op_ack_tgl,
    input      [63:0]  snap_total_events,
    input      [31:0]  snap_pileup,
    input      [31:0]  snap_dropped,
    input      [31:0]  snap_sat,
    input      [63:0]  snap_total_ticks,
    input      [63:0]  snap_busy_ticks,
    input      [15:0]  snap_baseline,
    input      [47:0]  snap_ts,
    input      [7:0]   snap_flags,

    output             ev_rd,
    input      [71:0]  ev_dout,
    input              ev_empty,

    input              wave_done_tgl,
    input      [11:0]  wave_start,
    input      [12:0]  wave_depth,
    input      [15:0]  wave_pretrig,
    input      [47:0]  wave_ts,
    input      [7:0]   wave_src,
    output     [11:0]  wave_rd_addr,
    input      [15:0]  wave_rd_data,

    output             led_host,
    output             led_run
);
    `include "mca_regs.vh"

    localparam [15:0] PORT_CMD = 16'd5000, PORT_EV = 16'd5001;
    localparam [7:0]  C_PING = 8'h01, C_WRITE = 8'h02, C_READ = 8'h03, C_CLEAR = 8'h04,
                      C_STREAM = 8'h05, C_WAVE = 8'h06, C_STATUS = 8'h07, C_RESET = 8'h08,
                      C_NET = 8'h09;
    localparam [7:0]  ST_OK = 0, ST_BAD_CMD = 1, ST_BAD_ADDR = 2, ST_BAD_VALUE = 3, ST_BUSY = 4;

    // =====================================================================
    // 配置寄存器（影子）
    // =====================================================================
    reg [31:0] cfg [0:NCFG-1];
    integer i;
    initial for (i = 0; i < NCFG; i = i + 1) cfg[i] = cfg_default(i);
    genvar gi;
    generate
        for (gi = 0; gi < NCFG; gi = gi + 1) begin : g_cfg
            assign cfg_bus[32*gi +: 32] = cfg[gi];
        end
    endgenerate

    reg [47:0] local_mac = DEF_MAC;
    reg [31:0] local_ip  = DEF_IP;
    reg [47:0] host_mac  = 48'hFFFF_FFFF_FFFF;
    reg [31:0] host_ip   = 32'hFFFF_FFFF;
    reg        host_valid = 1'b0, host_lock = 1'b0;
    reg [6:0]  stream_n  = 7'd64;
    reg [15:0] stream_ms = 16'd1;
    reg [31:0] timeout_cyc = CYC_PER_MS;

    assign led_host = host_valid;
    assign led_run  = cfg[R_CTRL][0];

    // =====================================================================
    // 接收解析
    // =====================================================================
    wire        arp_valid, cmd_valid;
    wire [47:0] arp_sha, cmd_src_mac;
    wire [31:0] arp_spa, cmd_src_ip, cmd_addr, cmd_value;
    wire [15:0] cmd_src_port;
    wire [7:0]  cmd_code, cmd_seq;
    wire [31:0] rx_frames, rx_errors;

    eth_rx_parser u_rx (
        .clk(clk), .rst(rst), .local_mac(local_mac), .local_ip(local_ip), .cmd_port(PORT_CMD),
        .rd_en(rx_rd_en), .rd_data(rx_dout), .empty(rx_empty),
        .arp_valid(arp_valid), .arp_sha(arp_sha), .arp_spa(arp_spa),
        .cmd_valid(cmd_valid), .cmd_src_mac(cmd_src_mac), .cmd_src_ip(cmd_src_ip),
        .cmd_src_port(cmd_src_port), .cmd_code(cmd_code), .cmd_seq(cmd_seq),
        .cmd_addr(cmd_addr), .cmd_value(cmd_value),
        .rx_frames(rx_frames), .rx_errors(rx_errors)
    );

    // ARP 应答待发
    reg        arp_req = 1'b0;
    reg [47:0] arp_mac = 0;
    reg [31:0] arp_ip = 0;

    // 命令待处理（一项缓冲，忙时丢弃，MCU 超时重发）
    reg        cmd_pend = 1'b0;
    reg [47:0] p_mac = 0;
    reg [31:0] p_ip = 0, p_addr = 0, p_val = 0;
    reg [15:0] p_port = 0;
    reg [7:0]  p_cmd = 0, p_seq = 0;

    // =====================================================================
    // ADC 域握手
    // =====================================================================
    wire ack_s;
    sync_ff u_sync_ack (.clk(clk), .d(op_ack_tgl), .q(ack_s));
    wire op_done = (ack_s == op_tgl);

    // =====================================================================
    // 波形发送状态
    // =====================================================================
    wire wdone_s;
    sync_ff u_sync_wd (.clk(clk), .d(wave_done_tgl), .q(wdone_s));
    reg  wdone_d = 1'b0;
    reg  wave_armed = 1'b0;      // 已布防，等待捕获完成
    reg  wf_active = 1'b0;       // 正在分包发送
    reg  wf_req = 1'b0;
    reg [12:0] wf_off = 0, wf_n = 0;
    reg [31:0] wf_id = 0;
    reg [11:0] w_start = 0;
    reg [12:0] w_depth = 0;
    reg [15:0] w_pre = 0;
    reg [47:0] w_ts = 0;
    reg [7:0]  w_src = 0;

    // =====================================================================
    // 状态字
    // =====================================================================
    wire [31:0] status_flags = {19'd0, host_lock, snap_flags[3], cfg[R_CTRL][3], snap_flags[2],
                                snap_flags[1], snap_flags[0], (wave_armed | wf_active),
                                cfg[R_CTRL][1], host_valid, speed, (speed != 2'd0), cfg[R_CTRL][0]};

    // =====================================================================
    // 控制状态机
    // =====================================================================
    localparam S_IDLE = 3'd0, S_EXEC = 3'd1, S_OPWAIT = 3'd2, S_POST = 3'd3,
               S_RESP = 3'd4, S_RESPWAIT = 3'd5, S_HBWAIT = 3'd6;
    localparam P_NONE = 2'd0, P_READ = 2'd1, P_STATUS = 2'd2;
    reg [2:0]  st = S_IDLE;
    reg [1:0]  post = P_NONE;
    reg [7:0]  r_status = 0;
    reg [31:0] r_value = 0;

    // 命令执行用
    reg [47:0] c_mac = 0;
    reg [31:0] c_ip = 0, c_addr = 0, c_val = 0;
    reg [15:0] c_port = 0;
    reg [7:0]  c_cmd = 0, c_seq = 0;
    wire [5:0] c_idx = c_addr[7:2];
    wire       c_cfg_space = (c_addr[31:2] < NCFG) && (c_addr[1:0] == 2'b00);
    wire [31:0] c_mask = cfg_mask(c_idx);
    wire [31:0] c_min  = cfg_min(c_idx);
    wire [31:0] c_max  = cfg_max(c_idx);

    // SET_NET：应答发出后再生效
    reg        net_pend = 1'b0;
    reg [2:0]  net_idx = 0;
    reg [31:0] net_val = 0;

    // MCRS 载荷寄存器
    reg        mcrs_req = 1'b0;
    reg [7:0]  rs_cmd = 0, rs_seq = 0, rs_status = 0;
    reg [31:0] rs_value = 0;
    reg [47:0] rs_mac = 0;
    reg [31:0] rs_ip = 0;
    reg [15:0] rs_port = 0;

    // MCST 载荷寄存器
    reg        mcst_req = 1'b0;
    reg [31:0] hb_seq = 0, hb_cnt = 0, hb_pkt = 0;
    reg [63:0] hb_tot = 0, hb_ticks = 0, hb_busy = 0;
    reg [31:0] hb_pu = 0, hb_drop = 0, hb_sat = 0, hb_status = 0;
    reg [31:0] hb_tmr = 0;
    reg        hb_due = 1'b0;

    // 引擎完成信号（见下文仲裁）
    wire [4:0] src_done;
    wire [31:0] pkt_seq;

    // 快照寄存器读
    reg [31:0] snap_rd;
    always @(*) begin
        case (c_addr[15:0])
            A_TOTAL_EVENTS_LO: snap_rd = snap_total_events[31:0];
            A_TOTAL_EVENTS_HI: snap_rd = snap_total_events[63:32];
            A_PILEUP_EVENTS:   snap_rd = snap_pileup;
            A_DROPPED_EVENTS:  snap_rd = snap_dropped;
            A_TOTAL_TICKS_LO:  snap_rd = snap_total_ticks[31:0];
            A_TOTAL_TICKS_HI:  snap_rd = snap_total_ticks[63:32];
            A_BUSY_TICKS_LO:   snap_rd = snap_busy_ticks[31:0];
            A_BUSY_TICKS_HI:   snap_rd = snap_busy_ticks[63:32];
            A_ADC_SAT_EVENTS:  snap_rd = snap_sat;
            A_BASELINE:        snap_rd = {16'd0, snap_baseline};
            A_TIMESTAMP_LO:    snap_rd = snap_ts[31:0];
            A_TIMESTAMP_HI:    snap_rd = {16'd0, snap_ts[47:32]};
            default:           snap_rd = 32'd0;
        endcase
    end
    wire c_snap_space = (c_addr[31:16] == 0) && (c_addr[15:0] >= A_TOTAL_EVENTS_LO) &&
                        (c_addr[15:0] <= A_TIMESTAMP_HI) && (c_addr[1:0] == 0);
    wire c_snap_hi = (c_addr[15:0] == A_TOTAL_EVENTS_HI) || (c_addr[15:0] == A_TOTAL_TICKS_HI) ||
                     (c_addr[15:0] == A_BUSY_TICKS_HI)   || (c_addr[15:0] == A_TIMESTAMP_HI);
    reg [31:0] eth_rd;
    reg        eth_rd_ok;
    always @(*) begin
        eth_rd_ok = (c_addr[31:16] == 0) && (c_addr[1:0] == 0);
        case (c_addr[15:0])
            A_PKT_SEQ:    eth_rd = pkt_seq;
            A_NET_STATUS: eth_rd = {26'd0, host_lock, host_valid, speed != 2'd0, 1'b0, speed};
            A_STREAM_N:   eth_rd = {25'd0, stream_n};
            A_STREAM_MS:  eth_rd = {16'd0, stream_ms};
            A_LOCAL_IP:   eth_rd = local_ip;
            A_HOST_IP:    eth_rd = host_ip;
            A_RX_FRAMES:  eth_rd = rx_frames;
            A_RX_ERRORS:  eth_rd = rx_errors;
            default: begin eth_rd = 32'd0; eth_rd_ok = 1'b0; end
        endcase
    end

    task start_op;
        input [2:0]  code;
        input [31:0] arg;
        begin
            op_code <= code;
            op_arg  <= arg;
            op_tgl  <= ~op_tgl;
        end
    endtask

    wire [15:0] wave_dc_len = 16'd256 << c_addr[18:16];

    always @(posedge clk) begin
        // ARP 请求锁存
        if (arp_valid && !arp_req) begin
            arp_req <= 1'b1; arp_mac <= arp_sha; arp_ip <= arp_spa;
        end
        if (src_done[0]) arp_req <= 1'b0;

        // 命令锁存
        if (cmd_valid && !cmd_pend) begin
            cmd_pend <= 1'b1;
            p_mac <= cmd_src_mac; p_ip <= cmd_src_ip; p_port <= cmd_src_port;
            p_cmd <= cmd_code; p_seq <= cmd_seq; p_addr <= cmd_addr; p_val <= cmd_value;
        end

        // 心跳定时
        if (hb_tmr >= HB_CYCLES - 1) begin
            hb_tmr <= 0;
            hb_due <= 1'b1;
        end else begin
            hb_tmr <= hb_tmr + 1'b1;
        end

        if (src_done[1]) mcrs_req <= 1'b0;
        if (src_done[2]) mcst_req <= 1'b0;

        if (rst) begin
            st <= S_IDLE;
            cmd_pend <= 1'b0; arp_req <= 1'b0; mcrs_req <= 1'b0; mcst_req <= 1'b0;
            hb_due <= 1'b0; hb_tmr <= 0; net_pend <= 1'b0;
            host_valid <= 1'b0; host_lock <= 1'b0;
            for (i = 0; i < NCFG; i = i + 1) cfg[i] <= cfg_default(i);
        end else case (st)
            S_IDLE: begin
                if (cmd_pend) begin
                    cmd_pend <= 1'b0;
                    c_mac <= p_mac; c_ip <= p_ip; c_port <= p_port;
                    c_cmd <= p_cmd; c_seq <= p_seq; c_addr <= p_addr; c_val <= p_val;
                    if (!host_lock) begin
                        host_mac <= p_mac; host_ip <= p_ip; host_valid <= 1'b1;
                    end
                    st <= S_EXEC;
                end else if (hb_due && host_valid && !mcst_req) begin
                    hb_due <= 1'b0;
                    start_op(OP_SNAP, 32'd0);
                    st <= S_HBWAIT;
                end else if (hb_due && !host_valid) begin
                    hb_due <= 1'b0;
                end
            end

            S_EXEC: begin
                r_status <= ST_OK;
                r_value  <= 32'd0;
                post     <= P_NONE;
                st       <= S_RESP;
                case (c_cmd)
                    C_PING: r_value <= MCA_VERSION;
                    C_WRITE: begin
                        if (!c_cfg_space || c_mask == 0) begin
                            r_status <= ST_BAD_ADDR;
                        end else if ((c_val & ~c_mask) != 0 || c_val < c_min || c_val > c_max) begin
                            r_status <= ST_BAD_VALUE;
                        end else begin
                            cfg[c_idx] <= c_val;
                            r_value <= c_val;
                            start_op(OP_CFG, 32'd0);
                            st <= S_OPWAIT;
                        end
                    end
                    C_READ: begin
                        if (c_cfg_space) begin
                            if (c_idx == R_ID)            r_value <= MCA_ID;
                            else if (c_idx == R_VERSION)  r_value <= MCA_VERSION;
                            else if (c_idx == R_STATUS) begin
                                post <= P_STATUS; start_op(OP_SNAP, 32'd0); st <= S_OPWAIT;
                            end
                            else if (c_mask != 0)         r_value <= cfg[c_idx];
                            else                          r_status <= ST_BAD_ADDR;
                        end else if (c_snap_space) begin
                            if (c_snap_hi) r_value <= snap_rd;
                            else begin post <= P_READ; start_op(OP_SNAP, 32'd0); st <= S_OPWAIT; end
                        end else if (eth_rd_ok) begin
                            r_value <= eth_rd;
                        end else begin
                            r_status <= ST_BAD_ADDR;
                        end
                    end
                    C_CLEAR: begin
                        start_op(OP_CLEAR, 32'd0);
                        st <= S_OPWAIT;
                    end
                    C_STREAM: begin
                        if (c_addr == 0 || c_addr > 64 || c_val == 0 || c_val > 10000) begin
                            r_status <= ST_BAD_VALUE;
                        end else begin
                            stream_n    <= c_addr[6:0];
                            stream_ms   <= c_val[15:0];
                            timeout_cyc <= c_val[15:0] * CYC_PER_MS;
                        end
                    end
                    C_WAVE: begin
                        if (wf_active) begin
                            r_status <= ST_BUSY;
                        end else if (c_addr[23:16] > 8'd4 || c_addr[15:0] >= wave_dc_len) begin
                            r_status <= ST_BAD_VALUE;
                        end else begin
                            wave_armed <= 1'b1;
                            start_op(OP_ARM, c_addr);
                            st <= S_OPWAIT;
                        end
                    end
                    C_STATUS: begin
                        post <= P_STATUS; start_op(OP_SNAP, 32'd0); st <= S_OPWAIT;
                    end
                    C_RESET: begin
                        for (i = 0; i < NCFG; i = i + 1) cfg[i] <= cfg_default(i);
                        stream_n <= 7'd64; stream_ms <= 16'd1; timeout_cyc <= CYC_PER_MS;
                        wave_armed <= 1'b0;
                        start_op(OP_RESET, 32'd0);
                        st <= S_OPWAIT;
                    end
                    C_NET: begin
                        if (c_addr > 7) r_status <= ST_BAD_ADDR;
                        else begin
                            net_pend <= 1'b1; net_idx <= c_addr[2:0]; net_val <= c_val;
                            r_value <= c_val;
                        end
                    end
                    default: r_status <= ST_BAD_CMD;
                endcase
            end

            S_OPWAIT: if (op_done) st <= S_POST;

            S_POST: begin
                if (post == P_READ)   r_value <= snap_rd;
                if (post == P_STATUS) r_value <= status_flags;
                st <= S_RESP;
            end

            S_RESP: if (!mcrs_req) begin
                rs_cmd <= c_cmd; rs_seq <= c_seq; rs_status <= r_status; rs_value <= r_value;
                rs_mac <= c_mac; rs_ip <= c_ip; rs_port <= c_port;
                mcrs_req <= 1'b1;
                st <= S_RESPWAIT;
            end

            S_RESPWAIT: if (src_done[1]) begin
                if (net_pend) begin
                    net_pend <= 1'b0;
                    case (net_idx)
                        3'd0: local_ip  <= net_val;
                        3'd1: local_mac[47:32] <= net_val[15:0];
                        3'd2: local_mac[31:0]  <= net_val;
                        3'd3: host_ip   <= net_val;
                        3'd4: host_mac[47:32]  <= net_val[15:0];
                        3'd5: host_mac[31:0]   <= net_val;
                        3'd6: host_valid <= net_val[0];
                        default: host_lock <= net_val[0];
                    endcase
                end
                st <= S_IDLE;
            end

            S_HBWAIT: if (op_done) begin
                hb_seq    <= hb_cnt;
                hb_cnt    <= hb_cnt + 1'b1;
                hb_pkt    <= pkt_seq;
                hb_tot    <= snap_total_events;
                hb_pu     <= snap_pileup;
                hb_drop   <= snap_dropped;
                hb_ticks  <= snap_total_ticks;
                hb_busy   <= snap_busy_ticks;
                hb_sat    <= snap_sat;
                hb_status <= status_flags;
                mcst_req  <= 1'b1;
                st <= S_IDLE;
            end

            default: st <= S_IDLE;
        endcase

        // 波形捕获完成 → 分包发送
        wdone_d <= wdone_s;
        if (rst) begin
            wave_armed <= 1'b0; wf_active <= 1'b0; wf_req <= 1'b0;
        end else begin
            if (wdone_s != wdone_d) begin
                wave_armed <= 1'b0;
                wf_active  <= 1'b1;
                wf_off     <= 0;
                wf_id      <= wf_id + 1'b1;
                w_start <= wave_start; w_depth <= wave_depth; w_pre <= wave_pretrig;
                w_ts <= wave_ts; w_src <= wave_src;
            end else if (wf_active && !wf_req && host_valid) begin
                wf_n   <= (w_depth - wf_off > 13'd256) ? 13'd256 : (w_depth - wf_off);
                wf_req <= 1'b1;
            end
            if (src_done[4]) begin
                wf_req <= 1'b0;
                wf_off <= wf_off + wf_n;
                if (wf_off + wf_n >= w_depth) wf_active <= 1'b0;
            end
        end
    end

    // =====================================================================
    // 事件打包
    // =====================================================================
    wire        ev_req;
    wire [9:0]  ev_len;
    wire [7:0]  ev_pl;
    wire [9:0]  pl_addr;
    mcev_packer u_pack (
        .clk(clk), .rst(rst), .enable(host_valid),
        .n_max(stream_n), .timeout_cyc(timeout_cyc),
        .fifo_rd(ev_rd), .fifo_dout(ev_dout), .fifo_empty(ev_empty),
        .req(ev_req), .done(src_done[3]), .len(ev_len),
        .pl_addr(pl_addr), .pl_data(ev_pl), .seq(pkt_seq)
    );

    // =====================================================================
    // 载荷读口（均为 1 拍延迟）
    // =====================================================================
    reg [7:0] rs_pl = 0, hb_pl = 0;
    always @(posedge clk) begin
        case (pl_addr[3:0])
            0: rs_pl <= "M"; 1: rs_pl <= "C"; 2: rs_pl <= "R"; 3: rs_pl <= "S";
            4: rs_pl <= rs_cmd; 5: rs_pl <= rs_seq; 6: rs_pl <= rs_status; 7: rs_pl <= 8'h00;
            8: rs_pl <= rs_value[31:24]; 9: rs_pl <= rs_value[23:16];
            10: rs_pl <= rs_value[15:8]; 11: rs_pl <= rs_value[7:0];
            default: rs_pl <= 8'h00;
        endcase
        case (pl_addr[5:0])
            0: hb_pl <= "M"; 1: hb_pl <= "C"; 2: hb_pl <= "S"; 3: hb_pl <= "T";
            4: hb_pl <= 8'h01;
            8:  hb_pl <= hb_seq[31:24];  9: hb_pl <= hb_seq[23:16];  10: hb_pl <= hb_seq[15:8];  11: hb_pl <= hb_seq[7:0];
            12: hb_pl <= hb_pkt[31:24]; 13: hb_pl <= hb_pkt[23:16]; 14: hb_pl <= hb_pkt[15:8]; 15: hb_pl <= hb_pkt[7:0];
            16: hb_pl <= hb_tot[63:56]; 17: hb_pl <= hb_tot[55:48]; 18: hb_pl <= hb_tot[47:40]; 19: hb_pl <= hb_tot[39:32];
            20: hb_pl <= hb_tot[31:24]; 21: hb_pl <= hb_tot[23:16]; 22: hb_pl <= hb_tot[15:8];  23: hb_pl <= hb_tot[7:0];
            24: hb_pl <= hb_pu[31:24];  25: hb_pl <= hb_pu[23:16];  26: hb_pl <= hb_pu[15:8];   27: hb_pl <= hb_pu[7:0];
            28: hb_pl <= hb_drop[31:24]; 29: hb_pl <= hb_drop[23:16]; 30: hb_pl <= hb_drop[15:8]; 31: hb_pl <= hb_drop[7:0];
            32: hb_pl <= hb_ticks[63:56]; 33: hb_pl <= hb_ticks[55:48]; 34: hb_pl <= hb_ticks[47:40]; 35: hb_pl <= hb_ticks[39:32];
            36: hb_pl <= hb_ticks[31:24]; 37: hb_pl <= hb_ticks[23:16]; 38: hb_pl <= hb_ticks[15:8];  39: hb_pl <= hb_ticks[7:0];
            40: hb_pl <= hb_busy[63:56]; 41: hb_pl <= hb_busy[55:48]; 42: hb_pl <= hb_busy[47:40]; 43: hb_pl <= hb_busy[39:32];
            44: hb_pl <= hb_busy[31:24]; 45: hb_pl <= hb_busy[23:16]; 46: hb_pl <= hb_busy[15:8];  47: hb_pl <= hb_busy[7:0];
            48: hb_pl <= hb_sat[31:24];  49: hb_pl <= hb_sat[23:16];  50: hb_pl <= hb_sat[15:8];   51: hb_pl <= hb_sat[7:0];
            52: hb_pl <= hb_status[31:24]; 53: hb_pl <= hb_status[23:16]; 54: hb_pl <= hb_status[15:8]; 55: hb_pl <= hb_status[7:0];
            default: hb_pl <= 8'h00;
        endcase
    end

    // MCWF：24 B 头 + 2 B × n 采样（BRAM 读 1 拍延迟）
    wire [9:0]  wa = pl_addr - 10'd24;
    assign wave_rd_addr = w_start + wf_off[11:0] + {3'd0, wa[9:1]};
    reg        wf_hdr_d = 1'b0, wf_lo_d = 1'b0;
    reg [7:0]  wf_hb = 0;
    wire [15:0] wf_nn = {3'd0, wf_n};
    always @(posedge clk) begin
        wf_hdr_d <= (pl_addr < 10'd24);
        wf_lo_d  <= wa[0];
        case (pl_addr[4:0])
            0: wf_hb <= "M"; 1: wf_hb <= "C"; 2: wf_hb <= "W"; 3: wf_hb <= "F";
            4: wf_hb <= 8'h01; 5: wf_hb <= w_src;
            6: wf_hb <= wf_nn[15:8]; 7: wf_hb <= wf_nn[7:0];
            8: wf_hb <= wf_id[31:24]; 9: wf_hb <= wf_id[23:16]; 10: wf_hb <= wf_id[15:8]; 11: wf_hb <= wf_id[7:0];
            12: wf_hb <= {3'd0, wf_off[12:8]}; 13: wf_hb <= wf_off[7:0];
            14: wf_hb <= {3'd0, w_depth[12:8]}; 15: wf_hb <= w_depth[7:0];
            16: wf_hb <= w_ts[31:24]; 17: wf_hb <= w_ts[23:16]; 18: wf_hb <= w_ts[15:8]; 19: wf_hb <= w_ts[7:0];
            20: wf_hb <= w_pre[15:8]; 21: wf_hb <= w_pre[7:0];
            default: wf_hb <= 8'h00;
        endcase
    end
    wire [7:0] wf_pl = wf_hdr_d ? wf_hb : (wf_lo_d ? wave_rd_data[7:0] : wave_rd_data[15:8]);

    // =====================================================================
    // 发送仲裁：ARP > MCRS > MCST > MCEV > MCWF
    // =====================================================================
    wire [4:0] reqs = {wf_req, ev_req, mcst_req, mcrs_req, arp_req};
    reg  [2:0] gnt = 3'd7;
    reg        start = 1'b0;
    reg        d_arp = 1'b0;
    reg [47:0] d_mac = 0;
    reg [31:0] d_ip = 0;
    reg [15:0] d_sp = 0, d_dp = 0;
    reg [9:0]  d_len = 0;
    wire       eng_busy, copy_done;
    reg        copying = 1'b0;

    always @(posedge clk) begin
        start <= 1'b0;
        if (rst) begin
            gnt <= 3'd7; copying <= 1'b0;
        end else if (!eng_busy && !start && !copying && reqs != 0) begin
            start <= 1'b1;
            copying <= 1'b1;
            d_arp <= 1'b0;
            if (reqs[0]) begin
                gnt <= 0; d_arp <= 1'b1; d_mac <= arp_mac; d_ip <= arp_ip;
                d_sp <= 0; d_dp <= 0; d_len <= 0;
            end else if (reqs[1]) begin
                gnt <= 1; d_mac <= rs_mac; d_ip <= rs_ip; d_sp <= PORT_CMD; d_dp <= rs_port; d_len <= 10'd16;
            end else if (reqs[2]) begin
                gnt <= 2; d_mac <= host_mac; d_ip <= host_ip; d_sp <= PORT_CMD; d_dp <= PORT_CMD; d_len <= 10'd64;
            end else if (reqs[3]) begin
                gnt <= 3; d_mac <= host_mac; d_ip <= host_ip; d_sp <= PORT_EV; d_dp <= PORT_EV; d_len <= ev_len;
            end else begin
                gnt <= 4; d_mac <= host_mac; d_ip <= host_ip; d_sp <= PORT_EV; d_dp <= PORT_EV;
                d_len <= 10'd24 + {wf_n[8:0], 1'b0};
            end
        end else if (copy_done) begin
            copying <= 1'b0;
        end
    end
    assign src_done = copy_done ? (5'b00001 << gnt) : 5'b0;

    reg [7:0] pl_mux;
    always @(*) begin
        case (gnt)
            3'd1: pl_mux = rs_pl;
            3'd2: pl_mux = hb_pl;
            3'd3: pl_mux = ev_pl;
            3'd4: pl_mux = wf_pl;
            default: pl_mux = 8'h00;
        endcase
    end

    eth_tx_engine u_tx (
        .clk(clk), .rst(rst), .local_mac(local_mac), .local_ip(local_ip),
        .start(start), .is_arp(d_arp), .dst_mac(d_mac), .dst_ip(d_ip),
        .src_port(d_sp), .dst_port(d_dp), .pl_len(d_len),
        .pl_addr(pl_addr), .pl_data(pl_mux), .busy(eng_busy), .copy_done(copy_done),
        .byte_en(byte_en), .tx_en(tx_en), .tx_data(tx_data)
    );
endmodule
