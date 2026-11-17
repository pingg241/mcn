`timescale 1ns / 1ps
// pulse_proc 单元仿真：测试脉冲源 → 脉冲处理，事件写入 events.txt
module tb_pulse;
    reg clk = 0;
    always #15.625 clk = ~clk;   // 32 MHz
    reg rst = 1;

    // 参数（与寄存器默认值一致）
    localparam [5:0]  FAST_K = 8,  FAST_GAP = 8;
    localparam [15:0] THR = 64;
    localparam [9:0]  SLOW_K = 64;
    localparam [7:0]  SLOW_M = 32;
    localparam [23:0] PZ_M = 1023 * 256;
    localparam [11:0] PEAK_DELAY = 63, PEAK_WIN = 42, PU_WIN = 112;
    localparam [11:0] BLR_HOLD = 184;

    reg [31:0] tp_rate;
    reg [15:0] tp_amp;
    reg [1:0]  tp_mode;
    reg [3:0]  tp_noise;
    reg        run = 0;

    wire [7:0] smp;
    wire fire;
    test_pulser u_tp (
        .clk(clk), .rst(rst), .rate(tp_rate), .amp(tp_amp), .mode(tp_mode),
        .noise(tp_noise), .decay(4'd10), .rise(4'd3), .base(8'd16), .neg(1'b0),
        .sample(smp), .fire(fire)
    );

    wire        ev_valid;
    wire [47:0] ev_ts;
    wire [15:0] ev_energy;
    wire [7:0]  ev_flags;
    wire [47:0] ts;
    wire [63:0] total_events, total_ticks, busy_ticks;
    wire [31:0] pileup_events, dropped_events, sat_events;
    wire [15:0] bl_in;
    wire ovf_seen, sat_seen, bl_bad, inhibit, trig;
    wire [15:0] w_x, w_ff, w_e;

    pulse_proc u_pp (
        .clk(clk), .rst(rst),
        .smp(smp), .smp_otr(1'b0), .src_test(1'b1),
        .run(run), .pol_neg(1'b0), .peak_fixed(1'b0),
        .fast_k(FAST_K), .fast_gap(FAST_GAP), .thr(THR),
        .slow_k(SLOW_K), .slow_m(SLOW_M), .pz_m(PZ_M),
        .peak_delay(PEAK_DELAY), .peak_win(PEAK_WIN), .pu_win(PU_WIN),
        .blr_shift(4'd9), .blr_hold(BLR_HOLD),
        .e_gain(16'd32768), .e_shift(6'd17),
        .bl_min(8'd0), .bl_max(8'd255), .tot_max(12'd0),
        .flush_req(1'b0), .clr_cnt(1'b0),
        .ev_valid(ev_valid), .ev_ts(ev_ts), .ev_energy(ev_energy), .ev_flags(ev_flags),
        .ev_full(1'b0), .pu_discard(1'b0),
        .ts(ts), .total_events(total_events), .pileup_events(pileup_events),
        .dropped_events(dropped_events), .sat_events(sat_events),
        .total_ticks(total_ticks), .busy_ticks(busy_ticks),
        .ovf_seen(ovf_seen), .sat_seen(sat_seen), .bl_in(bl_in), .bl_bad(bl_bad),
        .inhibit(inhibit), .trig_out(trig), .w_x(w_x), .w_ff(w_ff), .w_e(w_e)
    );

    integer fd, fw, ff, nfire;
    initial begin
        fd = $fopen("events.txt", "w");
        fw = $fopen("wave.txt", "w");
        ff = $fopen("fires.txt", "w");
        nfire = 0;
        tp_rate  = 32'd1342177 * 2;   // 约 20 kcps
        tp_amp   = 16'h6400;          // 100.0 LSB
        tp_mode  = 2'd0;
        tp_noise = 4'd1;
        if ($value$plusargs("rate=%d", tp_rate)) ;
        if ($value$plusargs("amp=%d", tp_amp)) ;
        if ($value$plusargs("mode=%d", tp_mode)) ;
        if ($value$plusargs("noise=%d", tp_noise)) ;
        repeat (10) @(posedge clk);
        rst = 0;
        @(negedge inhibit);
        run = 1;
        repeat (32000 * 10) @(posedge clk);   // 10 ms
        run = 0;
        repeat (500) @(posedge clk);
        $display("fires=%0d total=%0d pileup=%0d dropped=%0d sat=%0d ticks=%0d busy=%0d bl=%0d/256",
                 nfire, total_events, pileup_events, dropped_events, sat_events,
                 total_ticks, busy_ticks, bl_in);
        $fclose(fd); $fclose(fw); $fclose(ff);
        $finish;
    end

    always @(posedge clk) begin
        if (run && fire) begin nfire = nfire + 1; $fwrite(ff, "%0d\n", ts); end
        if (ev_valid) $fwrite(fd, "%0d %0d %0d\n", ev_ts, ev_energy, ev_flags);
        if (run && ts < 48'd40000) $fwrite(fw, "%0d %0d %0d %0d %0d\n", ts, smp, $signed(w_ff), $signed(w_e), trig);
    end
endmodule
