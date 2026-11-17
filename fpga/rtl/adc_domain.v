`timescale 1ns / 1ps
// ADC 时钟域：配置锁存、eth→adc 操作握手、测试源、脉冲处理、计数快照、波形捕获
module adc_domain (
    input                  clk,
    input                  rst,
    input      [7:0]       adc_sample,
    input                  adc_otr,

    // 来自以太网域（准静态 + 握手）
    input      [32*24-1:0] cfg_bus,
    input                  op_tgl,
    input      [2:0]       op_code,
    input      [31:0]      op_arg,
    output reg             op_ack_tgl = 1'b0,

    // 快照（ack 后保持稳定，供以太网域读取）
    output reg [63:0]      snap_total_events = 0,
    output reg [31:0]      snap_pileup = 0,
    output reg [31:0]      snap_dropped = 0,
    output reg [31:0]      snap_sat = 0,
    output reg [63:0]      snap_total_ticks = 0,
    output reg [63:0]      snap_busy_ticks = 0,
    output reg [15:0]      snap_baseline = 0,
    output reg [47:0]      snap_ts = 0,
    output reg [7:0]       snap_flags = 0,      // {3'b0, wave_busy, inhibit, bl_bad, sat_seen, ovf_seen}

    // 事件流 → 异步 FIFO
    output                 ev_valid,
    output     [71:0]      ev_data,
    input                  ev_full,

    // 波形
    input                  rd_clk,
    input      [11:0]      wave_rd_addr,
    output     [15:0]      wave_rd_data,
    output                 wave_done_tgl,
    output     [11:0]      wave_start,
    output     [12:0]      wave_depth,
    output     [15:0]      wave_pretrig,
    output     [47:0]      wave_ts,
    output     [7:0]       wave_src,

    output                 led_event
);
    `include "mca_regs.vh"

    // ---------------- 操作握手 ----------------
    wire op_s;
    sync_ff u_sync_op (.clk(clk), .d(op_tgl), .q(op_s));
    reg  op_d = 1'b0, ack_pend = 1'b0;
    wire op_edge = op_s ^ op_d;

    reg [32*24-1:0] cfg_r;
    reg  soft_rst = 1'b0, flush_req = 1'b0, clr_cnt = 1'b0, arm = 1'b0;
    reg  [31:0] arm_arg = 0;

    wire [63:0] total_events, total_ticks, busy_ticks;
    wire [31:0] pileup_events, dropped_events, sat_events;
    wire [47:0] ts;
    wire [15:0] bl_in;
    wire ovf_seen, sat_seen, bl_bad, inhibit, wave_busy;

    integer k;
    initial for (k = 0; k < 24; k = k + 1) cfg_r[32*k +: 32] = cfg_default(k);

    always @(posedge clk) begin
        op_d      <= op_s;
        soft_rst  <= 1'b0;
        flush_req <= 1'b0;
        clr_cnt   <= 1'b0;
        arm       <= 1'b0;
        ack_pend  <= 1'b0;
        if (ack_pend) op_ack_tgl <= ~op_ack_tgl;
        if (rst) begin
            cfg_r <= cfg_bus;
        end else if (op_edge) begin
            ack_pend <= 1'b1;
            case (op_code)
                OP_CFG:   begin cfg_r <= cfg_bus; flush_req <= 1'b1; end
                OP_RESET: begin cfg_r <= cfg_bus; soft_rst  <= 1'b1; end
                OP_CLEAR: clr_cnt <= 1'b1;
                OP_ARM:   begin arm <= 1'b1; arm_arg <= op_arg; end
                OP_SNAP: begin
                    snap_total_events <= total_events;
                    snap_pileup       <= pileup_events;
                    snap_dropped      <= dropped_events;
                    snap_sat          <= sat_events;
                    snap_total_ticks  <= total_ticks;
                    snap_busy_ticks   <= busy_ticks;
                    snap_baseline     <= bl_in;
                    snap_ts           <= ts;
                    snap_flags        <= {3'b0, wave_busy, inhibit, bl_bad, sat_seen, ovf_seen};
                end
                default: ;
            endcase
        end
    end

    // ---------------- 配置字段 ----------------
    wire [31:0] ctrl    = cfg_r[32*R_CTRL +: 32];
    wire        run     = ctrl[0];
    wire        src_tst = ctrl[1];
    wire        pol_neg = ctrl[2];
    wire        pu_disc = ctrl[3];
    wire        pk_fix  = ctrl[4];
    wire [31:0] tp_cfg  = cfg_r[32*R_TP_CFG +: 32];
    wire [15:0] bl_win  = cfg_r[32*R_BL_WIN +: 16];

    wire prst = rst | soft_rst;

    // ---------------- 测试源与输入选择 ----------------
    wire [7:0] tp_sample;
    wire       tp_fire;
    test_pulser u_tp (
        .clk(clk), .rst(prst),
        .rate (cfg_r[32*R_TP_RATE +: 32]),
        .amp  (cfg_r[32*R_TP_AMP  +: 16]),
        .mode (tp_cfg[1:0]),
        .noise(tp_cfg[7:4]),
        .decay(tp_cfg[11:8]),
        .rise (tp_cfg[15:12]),
        .base (cfg_r[32*R_TP_BASE +: 8]),
        .neg  (pol_neg),
        .sample(tp_sample), .fire(tp_fire)
    );

    reg [7:0] smp = 0;
    reg       otr = 1'b0;
    always @(posedge clk) begin
        smp <= src_tst ? tp_sample : adc_sample;
        otr <= src_tst ? 1'b0 : adc_otr;
    end

    // ---------------- 脉冲处理 ----------------
    wire        pp_valid;
    wire [47:0] pp_ts;
    wire [15:0] pp_energy;
    wire [7:0]  pp_flags;
    wire        trig;
    wire [15:0] w_x, w_ff, w_e;

    pulse_proc u_pp (
        .clk(clk), .rst(prst),
        .smp(smp), .smp_otr(otr), .src_test(src_tst),
        .run(run), .pol_neg(pol_neg), .peak_fixed(pk_fix),
        .fast_k    (cfg_r[32*R_FAST_K       +: 6]),
        .fast_gap  (cfg_r[32*R_FAST_GAP     +: 6]),
        .thr       (cfg_r[32*R_THRESHOLD    +: 16]),
        .slow_k    (cfg_r[32*R_SLOW_K       +: 10]),
        .slow_m    (cfg_r[32*R_SLOW_M       +: 8]),
        .pz_m      (cfg_r[32*R_PZ_M         +: 24]),
        .peak_delay(cfg_r[32*R_PEAK_DELAY   +: 12]),
        .peak_win  (cfg_r[32*R_PEAK_WIN     +: 12]),
        .pu_win    (cfg_r[32*R_PILEUP_WIN   +: 12]),
        .blr_shift (cfg_r[32*R_BLR_SHIFT    +: 4]),
        .blr_hold  (cfg_r[32*R_BLR_HOLD     +: 12]),
        .e_gain    (cfg_r[32*R_ENERGY_GAIN  +: 16]),
        .e_shift   (cfg_r[32*R_ENERGY_SHIFT +: 6]),
        .bl_min    (bl_win[7:0]),
        .bl_max    (bl_win[15:8]),
        .tot_max   (cfg_r[32*R_TOT_MAX      +: 12]),
        .flush_req(flush_req), .clr_cnt(clr_cnt),
        .ev_valid(pp_valid), .ev_ts(pp_ts), .ev_energy(pp_energy), .ev_flags(pp_flags),
        .ev_full(ev_full), .pu_discard(pu_disc),
        .ts(ts), .total_events(total_events), .pileup_events(pileup_events),
        .dropped_events(dropped_events), .sat_events(sat_events),
        .total_ticks(total_ticks), .busy_ticks(busy_ticks),
        .ovf_seen(ovf_seen), .sat_seen(sat_seen), .bl_in(bl_in), .bl_bad(bl_bad),
        .inhibit(inhibit), .trig_out(trig),
        .w_x(w_x), .w_ff(w_ff), .w_e(w_e)
    );

    assign ev_valid = pp_valid;
    assign ev_data  = {pp_ts, pp_energy, pp_flags};

    // ---------------- 波形捕获 ----------------
    wave_capture u_wave (
        .clk(clk), .rst(prst),
        .arm(arm), .arm_src(arm_arg[31:24]), .arm_depth(arm_arg[18:16]), .arm_pretrig(arm_arg[15:0]),
        .trig(trig), .ts(ts),
        .s0({8'd0, smp}), .s1(w_x), .s2(w_ff), .s3(w_e),
        .busy(wave_busy), .done_tgl(wave_done_tgl),
        .start_addr(wave_start), .depth(wave_depth), .pretrig(wave_pretrig),
        .trig_ts(wave_ts), .src(wave_src),
        .rd_clk(rd_clk), .rd_addr(wave_rd_addr), .rd_data(wave_rd_data)
    );

    // 事件指示灯：展宽到约 30 ms
    reg [19:0] led_cnt = 0;
    always @(posedge clk) begin
        if (pp_valid) led_cnt <= 20'hFFFFF;
        else if (led_cnt != 0) led_cnt <= led_cnt - 1'b1;
    end
    assign led_event = (led_cnt != 0);
endmodule
