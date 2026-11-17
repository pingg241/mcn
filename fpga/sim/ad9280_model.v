`timescale 1ns / 1ps
// AD9280 行为模型：上升沿采样，3 级流水线，输出延时 T_OD。
// 模拟输入：基线 + 单极点指数衰减脉冲（衰减系数 1-2^-10，与默认 PZ 匹配）+ 一阶上升。
// 每 PERIOD 个采样注入一个脉冲，幅度按 AMPS 循环；注入记录写入 adc_inj.txt。
module ad9280_model #(
    parameter real T_OD   = 25.0,
    parameter real BASE   = 20.0,
    parameter      PERIOD = 1280,      // 40 µs
    parameter      START  = 2000
)(
    input            clk,
    output reg [7:0] data = 8'd0,
    output reg       otr = 1'b0
);
    real q = 0.0, y = 0.0, v;
    real amps [0:4];
    integer n = 0, ai = 0, code;
    reg [7:0] p0 = 0, p1 = 0, p2 = 0;
    reg       o0 = 0, o1 = 0, o2 = 0;
    integer fi;
    initial begin
        amps[0] = 20.0; amps[1] = 40.0; amps[2] = 60.0; amps[3] = 80.0; amps[4] = 100.0;
        fi = $fopen("adc_inj.txt", "w");
    end

    always @(posedge clk) begin
        n = n + 1;
        if (n >= START && ((n - START) % PERIOD) == 0) begin
            q = q + amps[ai];
            $fwrite(fi, "%0d %0d %f\n", $time, n, amps[ai]);
            $fflush(fi);
            ai = (ai + 1) % 5;
        end
        y = y + (q - y) / 8.0;            // 上升时间常数 8 采样
        q = q - q / 1024.0;               // 衰减
        v = BASE + y;
        code = $rtoi(v + 0.5);
        o0 <= (code > 255) || (code < 0);
        p0 <= (code > 255) ? 8'd255 : (code < 0 ? 8'd0 : code[7:0]);
        p1 <= p0; p2 <= p1;
        o1 <= o0; o2 <= o1;
        data <= #(T_OD) p2;
        otr  <= #(T_OD) o2;
    end
endmodule
