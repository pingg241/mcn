`timescale 1ns / 1ps
// 全系统仿真：顶层 Block Design（build/mca_sim 生成的仿真版：心跳 1 ms、PHY 复位 100 拍）
// + RGMII PHY 模型 + AD9280 模型。
// 激励帧来自 rx_frames.txt（由 tb_top_frames.py gen 生成），
// FPGA 发出的帧写入 tx_frames.txt（由 tb_top_frames.py check 检查）。
module tb_top;
    parameter GIGA = 1;

    reg sys_clk = 1'b0;
    always #10 sys_clk = ~sys_clk;           // 50 MHz
    reg sys_rst_n = 1'b0;

    wire       ad_clk, ad_otr;
    wire [7:0] ad_data;
    wire       eth_rxc, eth_rx_ctl, eth_txc, eth_tx_ctl, eth_rst_n;
    wire [3:0] eth_rxd, eth_txd, led;

    mca_block dut (
        .sys_clk(sys_clk), .sys_rst_n(sys_rst_n),
        .ad_clk(ad_clk), .ad_data(ad_data), .ad_otr(ad_otr),
        .eth_rxc(eth_rxc), .eth_rx_ctl(eth_rx_ctl), .eth_rxd(eth_rxd),
        .eth_txc(eth_txc), .eth_tx_ctl(eth_tx_ctl), .eth_txd(eth_txd),
        .eth_rst_n(eth_rst_n), .led(led)
    );

    ad9280_model u_adc (.clk(ad_clk), .data(ad_data), .otr(ad_otr));

    phy_model #(.GIGA(GIGA)) u_phy (
        .rxc(eth_rxc), .rx_ctl(eth_rx_ctl), .rxd(eth_rxd),
        .txc(eth_txc), .tx_ctl(eth_tx_ctl), .txd(eth_txd)
    );

    integer fd, r, i, len, b, nfr;
    reg [63:0] t;
    initial begin
        fd = $fopen("rx_frames.txt", "r");
        if (fd == 0) begin
            $display("找不到 rx_frames.txt");
            $finish;
        end
        nfr = 0;
        #200 sys_rst_n = 1'b1;
        forever begin
            r = $fscanf(fd, "%d %d", t, len);
            if (r != 2) begin
                $display("激励文件格式错误");
                $finish;
            end
            for (i = 0; i < len; i = i + 1) begin
                r = $fscanf(fd, "%h", b);
                u_phy.fbuf[i] = b[7:0];
            end
            if ($time < t) #(t - $time);
            if (len == 0) begin
                $display("[%0t] 仿真结束：发出 %0d 帧，收到 %0d 帧", $time, nfr, u_phy.got);
                $finish;
            end
            u_phy.send_frame(len);
            nfr = nfr + 1;
        end
    end

    // 进度
    initial begin
        forever begin
            #500_000;
            $display("[%0t] 收到 %0d 帧  led=%b", $time, u_phy.got, led);
        end
    end
endmodule
