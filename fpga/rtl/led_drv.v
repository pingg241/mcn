`timescale 1ns / 1ps
// 板上 4 个 LED：LED0 心跳闪烁（PHY 复位放开后才闪），LED1 链路（有 RXC），LED2 RUN，LED3 事件
module led_drv (
    input        clk,
    input        phy_ready,
    input  [1:0] speed,
    input        led_run,
    input        led_event,
    output [3:0] led
);
    reg [26:0] blink = 0;
    always @(posedge clk) blink <= blink + 1'b1;
    assign led = {led_event, led_run, (speed != 2'd0), blink[26] & phy_ready};
endmodule
