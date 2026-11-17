`timescale 1ns / 1ps
// 波形捕获：ADC 域写入 4096×16 环形缓冲，以太网域读出。
// 深度 = 256 << depth_code（0..4），含 pretrig 个触发前采样。
// src[1:0]：0 原始采样，1 极性校正后，2 快滤波，3 慢滤波（能量单位）；src[7]=1 不等脉冲立即触发。
module wave_capture (
    input             clk,
    input             rst,
    input             arm,          // 单拍：按下列参数重新布防
    input      [7:0]  arm_src,
    input      [2:0]  arm_depth,
    input      [15:0] arm_pretrig,
    input             trig,
    input      [47:0] ts,
    input      [15:0] s0, s1, s2, s3,

    output            busy,
    output reg        done_tgl = 1'b0,
    output reg [11:0] start_addr = 0,
    output reg [12:0] depth = 0,
    output reg [15:0] pretrig = 0,
    output reg [47:0] trig_ts = 0,
    output reg [7:0]  src = 0,

    input             rd_clk,
    input      [11:0] rd_addr,
    output reg [15:0] rd_data
);
    localparam S_IDLE = 2'd0, S_PRE = 2'd1, S_POST = 2'd2;
    reg [1:0]  st = S_IDLE;
    reg [11:0] wp = 0;
    reg [12:0] pre_cnt = 0;
    reg [12:0] post_cnt = 0;
    reg [15:0] mem [0:4095];

    reg [15:0] smp;
    always @(*) begin
        case (src[1:0])
            2'd0: smp = s0;
            2'd1: smp = s1;
            2'd2: smp = s2;
            default: smp = s3;
        endcase
    end

    assign busy = (st != S_IDLE);
    wire   pre_ok = (pre_cnt >= {1'b0, pretrig[11:0]});

    always @(posedge clk)
        if (st != S_IDLE) mem[wp] <= smp;

    always @(posedge clk) begin
        if (st != S_IDLE) wp <= wp + 1'b1;
        if (rst) begin
            st <= S_IDLE;
        end else if (arm) begin
            st      <= S_PRE;
            src     <= arm_src;
            depth   <= 13'd256 << arm_depth;
            pretrig <= arm_pretrig;
            pre_cnt <= 0;
        end else begin
            case (st)
                S_PRE: begin
                    if (pre_cnt != 13'h1FFF) pre_cnt <= pre_cnt + 1'b1;
                    if (pre_ok && (trig || src[7])) begin
                        start_addr <= wp - pretrig[11:0];
                        trig_ts    <= ts;
                        post_cnt   <= depth - pretrig[12:0] - 1'b1;
                        st         <= (depth - pretrig[12:0] == 13'd1) ? S_IDLE : S_POST;
                        if (depth - pretrig[12:0] == 13'd1) done_tgl <= ~done_tgl;
                    end
                end
                S_POST: begin
                    post_cnt <= post_cnt - 1'b1;
                    if (post_cnt == 13'd1) begin
                        st       <= S_IDLE;
                        done_tgl <= ~done_tgl;
                    end
                end
                default: ;
            endcase
        end
    end

    always @(posedge rd_clk) rd_data <= mem[rd_addr];
endmodule
