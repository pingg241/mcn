`timescale 1ns / 1ps
// 数字脉冲处理（ADC 时钟域，每拍一个采样）
//
//  x ──┬─ 快滤波 ff = Σx[n-kf+1..n] - Σx[n-lf-kf+1..n-lf] ── 过阈触发 / 时间戳 / 堆积检测
//      └─ Jordanov 梯形 + 极零补偿：
//           a = x[n] - x[n-k]
//           d = a[n] - a[n-l]           (l = k + m)
//           p = p + d
//           r = 256·p + Mq·d            (Mq = M·256，M = 1/(e^{Ts/τ}-1))
//           s = s + r                   (指数输入 A → 平顶高度 A·k·(M+1)·256)
//      s 减去空闲期跟踪的基线 (BLR) → 增益/移位 → e (能量单位)，在峰值窗口内取最大值
//
// 流水线：触发在 S4，e 在 S11，相对触发晚 7 拍，PEAK_DELAY 以触发拍为零点。
module pulse_proc (
    input             clk,
    input             rst,

    input      [7:0]  smp,          // 输入采样（ADC 或测试源）
    input             smp_otr,      // ADC 超量程
    input             src_test,     // 当前输入是测试源（事件打测试标志）

    // 配置（准静态，cfg 更新时伴随 flush）
    input             run,
    input             pol_neg,
    input             peak_fixed,   // 0 窗口内取最大；1 窗口中点定点采样
    input      [5:0]  fast_k,       // 1..63
    input      [5:0]  fast_gap,     // 0..63
    input      [15:0] thr,
    input      [9:0]  slow_k,       // 1..1023
    input      [7:0]  slow_m,       // 0..255
    input      [23:0] pz_m,         // Q16.8
    input      [11:0] peak_delay,
    input      [11:0] peak_win,     // >=1
    input      [11:0] pu_win,       // 堆积判别窗 W
    input      [3:0]  blr_shift,
    input      [11:0] blr_hold,
    input      [15:0] e_gain,
    input      [5:0]  e_shift,
    input      [7:0]  bl_min,
    input      [7:0]  bl_max,
    input      [11:0] tot_max,

    input             flush_req,    // 配置更新后冲刷滤波器
    input             clr_cnt,      // 清计数器

    // 事件输出
    output reg        ev_valid,
    output reg [47:0] ev_ts,
    output reg [15:0] ev_energy,
    output reg [7:0]  ev_flags,
    input             ev_full,      // 下游 FIFO 满（ev_valid 即 FIFO 写使能）
    input             pu_discard,   // 堆积事件不进 FIFO（仍计数）

    // 计数器
    output reg [47:0] ts,
    output reg [63:0] total_events,
    output reg [31:0] pileup_events,
    output reg [31:0] dropped_events,
    output reg [31:0] sat_events,
    output reg [63:0] total_ticks,
    output reg [63:0] busy_ticks,
    output reg        ovf_seen,
    output reg        sat_seen,
    output reg [15:0] bl_in,        // 输入基线 8.8 定点
    output            bl_bad,
    output            inhibit,

    // 触发与波形观察
    output            trig_out,
    output     [15:0] w_x,
    output     [15:0] w_ff,
    output     [15:0] w_e
);
    localparam FLUSH_LEN = 13'd4096;

    // ---------------- 冲刷 / 稳定控制 ----------------
    reg [12:0] flush_cnt = FLUSH_LEN;
    reg [12:0] settle_cnt = 0;
    wire flushing = (flush_cnt != 0);
    wire settling = (settle_cnt != 0);
    assign inhibit = flushing | settling;
    wire flush_last  = (flush_cnt == 13'd1);
    wire settle_last = (settle_cnt == 13'd1);

    wire [10:0] slow_l = {1'b0, slow_k} + {3'b0, slow_m};
    wire [6:0]  fast_l = {1'b0, fast_k} + {1'b0, fast_gap};
    wire [12:0] settle_len = {3'b0, slow_k} + {2'b0, slow_l} + {6'b0, fast_l} + 13'd64;

    always @(posedge clk) begin
        if (rst || flush_req) begin
            flush_cnt  <= FLUSH_LEN;
            settle_cnt <= 0;
        end else if (flushing) begin
            flush_cnt <= flush_cnt - 1'b1;
            if (flush_last) settle_cnt <= settle_len;
        end else if (settling) begin
            settle_cnt <= settle_cnt - 1'b1;
        end
    end

    // ---------------- S0：极性、饱和 ----------------
    reg [7:0] x0 = 0;
    reg       sat0 = 0;
    wire [7:0] xp = pol_neg ? ~smp : smp;
    always @(posedge clk) begin
        x0   <= flushing ? 8'd0 : xp;
        sat0 <= !flushing && (smp_otr || xp == 8'hFF);
    end

    // ---------------- S1：延时线 ----------------
    wire [7:0] x1, xk1, xf1, xkf1;
    delay_line #(.W(8), .AW(10)) u_dl1  (.clk(clk), .din(x0), .dly(slow_k),          .now(x1),  .del(xk1));
    delay_line #(.W(8), .AW(6))  u_dlf1 (.clk(clk), .din(x0), .dly(fast_k),          .now(xf1), .del(xkf1));

    // ---------------- S2 ----------------
    reg signed [8:0]  a2 = 0;
    reg        [15:0] fs2 = 0;
    always @(posedge clk) begin
        a2 <= $signed({1'b0, x1}) - $signed({1'b0, xk1});
        if (flush_last) fs2 <= 0;
        else            fs2 <= fs2 + {8'd0, xf1} - {8'd0, xkf1};
    end

    // ---------------- S3：延时线 ----------------
    wire [8:0]  a3, al3;
    wire [15:0] fs3, fsl3;
    delay_line #(.W(9),  .AW(11)) u_dl2  (.clk(clk), .din(a2),  .dly(slow_l), .now(a3),  .del(al3));
    delay_line #(.W(16), .AW(7))  u_dlf2 (.clk(clk), .din(fs2), .dly(fast_l), .now(fs3), .del(fsl3));

    // ---------------- S4：差分 / 快滤波 ----------------
    reg signed [9:0]  d4 = 0;
    reg signed [15:0] ff4 = 0;
    always @(posedge clk) begin
        d4  <= $signed(a3) - $signed(al3);
        ff4 <= fs3 - fsl3;
    end

    // ---------------- S5..S7：梯形累加 ----------------
    reg signed [23:0] p5 = 0;
    reg signed [34:0] md5 = 0;
    reg signed [35:0] r6 = 0;
    reg signed [47:0] s7 = 0;
    always @(posedge clk) begin
        if (flush_last) begin
            p5 <= 0; md5 <= 0; r6 <= 0; s7 <= 0;
        end else begin
            p5  <= p5 + d4;
            md5 <= $signed({1'b0, pz_m}) * d4;
            r6  <= $signed({p5, 8'd0}) + md5;
            s7  <= s7 + r6;
        end
    end

    // ---------------- 基线恢复 (BLR) 与能量换算 S8..S11 ----------------
    reg signed [63:0] blr_acc = 0;                 // 48 位整数 + 16 位小数
    wire signed [47:0] blr_int = blr_acc[63:16];
    reg signed [47:0] srel8 = 0;
    reg signed [24:0] sh9 = 0;
    reg signed [41:0] prod10 = 0;
    reg signed [17:0] e11 = 0;

    wire signed [47:0] shv = srel8 >>> e_shift;
    wire signed [27:0] pv  = prod10 >>> 14;
    always @(posedge clk) begin
        srel8  <= s7 - blr_int;
        sh9    <= (shv >  48'sd16777215) ?  25'sd16777215 :
                  (shv < -48'sd16777216) ? -25'sd16777216 : shv[24:0];
        prod10 <= sh9 * $signed({1'b0, e_gain});
        e11    <= (pv >  28'sd131071) ?  18'sd131071 :
                  (pv < -28'sd131072) ? -18'sd131072 : pv[17:0];
    end

    // ---------------- 触发 ----------------
    wire signed [16:0] thr_s  = $signed({1'b0, thr});
    wire signed [16:0] rearm_s = $signed({1'b0, thr - (thr >> 2)});
    wire above4 = ($signed({ff4[15], ff4}) >  thr_s);
    wire below4 = ($signed({ff4[15], ff4}) <  rearm_s);
    reg  armed = 1'b0;
    wire trig  = run && !inhibit && armed && above4;
    assign trig_out = trig;

    always @(posedge clk) begin
        if (rst || inhibit) armed <= 1'b0;
        else if (trig)      armed <= 1'b0;
        else if (below4)    armed <= 1'b1;
    end

    // 过阈时间过长（快滤波分辨不开的堆积）
    reg [11:0] tot_cnt = 0;
    reg        tot_hit = 1'b0;
    always @(posedge clk) begin
        tot_hit <= 1'b0;
        if (!above4) tot_cnt <= 0;
        else if (tot_cnt != 12'hFFF) begin
            tot_cnt <= tot_cnt + 1'b1;
            if (tot_max != 0 && tot_cnt == tot_max) tot_hit <= 1'b1;
        end
    end

    // 距上次触发的拍数（BLR 门控）
    reg [11:0] since_trig = 12'hFFF;
    always @(posedge clk) begin
        if (trig) since_trig <= 0;
        else if (since_trig != 12'hFFF) since_trig <= since_trig + 1'b1;
    end
    wire quiet = !inhibit && !above4 && (since_trig >= blr_hold);

    // BLR 与输入基线监测
    reg signed [24:0] bl_acc = 0;   // 8 位整数 + 16 位小数（最高位为符号位，恒为 0）
    always @(posedge clk) begin
        if (rst) begin
            blr_acc <= 0;
            bl_acc  <= 0;
        end else if (settle_last) begin
            blr_acc <= {s7, 16'd0};
            bl_acc  <= {x1, 16'd0};
        end else if (quiet) begin
            blr_acc <= blr_acc + ($signed({srel8, 16'd0}) >>> blr_shift);
            bl_acc  <= bl_acc + (($signed({1'b0, x1, 16'd0}) - bl_acc) >>> 6);
        end
        bl_in <= bl_acc[23:8];
    end
    assign bl_bad = (bl_in[15:8] < bl_min) || (bl_in[15:8] > bl_max);

    // ---------------- 事件：峰值窗口 / 堆积 ----------------
    wire [12:0] peak_end = {1'b0, peak_delay} + {1'b0, peak_win};
    wire [12:0] win_len  = (peak_end > {1'b0, pu_win}) ? peak_end : {1'b0, pu_win};
    wire [12:0] fix_pt   = {1'b0, peak_delay} + {2'b0, peak_win[11:1]};

    reg        pend = 1'b0;
    reg [12:0] cnt = 0;
    reg [47:0] pend_ts = 0;
    reg        pend_pu = 0, pend_sat = 0, pend_bl = 0, pend_test = 0;
    reg signed [17:0] pmax = 0;

    reg        emit = 1'b0;
    reg [47:0] emit_ts = 0;
    reg signed [17:0] emit_e = 0;
    reg [3:0]  emit_fl = 0;          // {pu, test, bl, sat}

    wire in_win = peak_fixed ? (cnt == fix_pt) : (cnt >= {1'b0, peak_delay} && cnt < peak_end);

    always @(posedge clk) begin
        emit <= 1'b0;
        if (rst) begin
            pend <= 1'b0;
        end else if (trig) begin
            if (pend) begin
                emit    <= 1'b1;
                emit_ts <= pend_ts;
                emit_e  <= pmax;
                emit_fl <= {1'b1, pend_test, pend_bl, pend_sat};
            end
            pend      <= 1'b1;
            pend_ts   <= ts;
            pend_pu   <= pend;
            pend_sat  <= sat0;
            pend_bl   <= bl_bad;
            pend_test <= src_test;
            cnt       <= 0;
            pmax      <= 0;
        end else if (pend) begin
            cnt <= cnt + 1'b1;
            if (in_win && (peak_fixed || e11 > pmax)) pmax <= e11;
            if (sat0)    pend_sat <= 1'b1;
            if (tot_hit) pend_pu  <= 1'b1;
            if (cnt + 1'b1 >= win_len) begin
                pend    <= 1'b0;
                emit    <= 1'b1;
                emit_ts <= pend_ts;
                emit_e  <= (in_win && (peak_fixed || e11 > pmax)) ? e11 : pmax;
                emit_fl <= {pend_pu | tot_hit, pend_test, pend_bl, pend_sat | sat0};
            end
        end
    end

    // 事件输出与计数
    wire [15:0] emit_energy = (emit_e < 0) ? 16'd0 :
                              (emit_e > 18'sd65535) ? 16'hFFFF : emit_e[15:0];
    wire        emit_keep   = !(pu_discard && emit_fl[3]);

    // 忙时间：每个触发贡献 [t-W, t+W] 的并集增量 min(gap, 2W)
    wire [12:0] w2 = {pu_win, 1'b0};
    reg  [12:0] gap_cnt = 0;

    always @(posedge clk) begin
        ev_valid <= 1'b0;
        if (rst) begin
            ts <= 0;
        end else begin
            ts <= ts + 1'b1;
        end

        if (rst || clr_cnt) begin
            total_events   <= 0;
            pileup_events  <= 0;
            dropped_events <= 0;
            sat_events     <= 0;
            total_ticks    <= 0;
            busy_ticks     <= 0;
            ovf_seen       <= 1'b0;
            sat_seen       <= 1'b0;
            gap_cnt        <= {1'b0, pu_win};
        end else begin
            if (run) total_ticks <= total_ticks + 1'b1;

            if (!run || inhibit) begin
                gap_cnt <= {1'b0, pu_win};
                if (run) busy_ticks <= busy_ticks + 1'b1;
            end else if (trig) begin
                busy_ticks <= busy_ticks + ((gap_cnt < w2) ? gap_cnt : w2);
                gap_cnt    <= 0;
            end else if (gap_cnt < w2) begin
                gap_cnt <= gap_cnt + 1'b1;
            end

            if (emit) begin
                total_events <= total_events + 1'b1;
                if (emit_fl[3]) pileup_events <= pileup_events + 1'b1;
                if (emit_fl[0]) begin
                    sat_events <= sat_events + 1'b1;
                    sat_seen   <= 1'b1;
                end
                if (emit_keep) ev_valid <= 1'b1;
            end
            // FIFO 满时写入被丢弃，在写入拍计数
            if (ev_valid && ev_full) begin
                dropped_events <= dropped_events + 1'b1;
                ovf_seen       <= 1'b1;
            end
        end
        ev_ts     <= emit_ts;
        ev_energy <= emit_energy;
        ev_flags  <= {4'd0, emit_fl};
    end

    assign w_x  = {8'd0, x0};
    assign w_ff = ff4;
    assign w_e  = (e11 > 18'sd32767) ? 16'sd32767 : (e11 < -18'sd32768) ? -16'sd32768 : e11[15:0];
endmodule
