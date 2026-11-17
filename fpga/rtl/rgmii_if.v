`timescale 1ns / 1ps
// RGMII 物理层接口，支持 1000/100/10 Mbps。
//  RX：BUFIO 采样 IDDR + BUFG 逻辑时钟，识别前导/SFD，帧字节经异步 FIFO 送到 clk_eth 域
//      FIFO 条目 {last, err, data[7:0]}
//  TX：clk_eth 域，每字节一个 byte_en；百兆/十兆按半字节展宽，TXC 由 ODDR 产生
//  速率：测量 RXC 频率（125 / 25 / 2.5 MHz）
module rgmii_if #(
    parameter IDELAY_VALUE = 0,
    parameter TXC_90       = 1      // 1：TXC 用 90° 时钟（PHY 不加 TX 延时时需要）
)(
    input            clk_eth,
    input            clk_eth90,
    input            clk_ref,       // 200 MHz IDELAYCTRL 参考
    input            rst_eth,

    input            rgmii_rxc,
    input            rgmii_rx_ctl,
    input  [3:0]     rgmii_rxd,
    output           rgmii_txc,
    output           rgmii_tx_ctl,
    output [3:0]     rgmii_txd,

    // RX 帧字节（clk_eth 域）
    input            rx_rd_en,
    output [9:0]     rx_dout,
    output           rx_empty,

    // TX（clk_eth 域）：tx_en/tx_data 为"当前字节"，byte_en 拍消费
    input            tx_en,
    input  [7:0]     tx_data,
    output reg       byte_en,

    output reg [1:0] speed          // 0 无时钟，1 10M，2 100M，3 1000M
);
    // =====================================================================
    // RX 采样
    // =====================================================================
    wire rxc_io, rx_clk;
    BUFIO u_bufio (.I(rgmii_rxc), .O(rxc_io));
    BUFG  u_bufg  (.I(rgmii_rxc), .O(rx_clk));

    (* IODELAY_GROUP = "rgmii_rx" *)
    IDELAYCTRL u_idelayctrl (.REFCLK(clk_ref), .RST(rst_eth), .RDY());

    wire [4:0] rx_pin = {rgmii_rx_ctl, rgmii_rxd};
    wire [4:0] rx_dly, rx_q1, rx_q2;
    genvar gi;
    generate
        for (gi = 0; gi < 5; gi = gi + 1) begin : g_rx
            (* IODELAY_GROUP = "rgmii_rx" *)
            IDELAYE2 #(
                .IDELAY_TYPE("FIXED"), .IDELAY_VALUE(IDELAY_VALUE),
                .DELAY_SRC("IDATAIN"), .REFCLK_FREQUENCY(200.0),
                .HIGH_PERFORMANCE_MODE("TRUE"), .SIGNAL_PATTERN("DATA")
            ) u_dly (
                .IDATAIN(rx_pin[gi]), .DATAOUT(rx_dly[gi]),
                .C(1'b0), .CE(1'b0), .INC(1'b0), .LD(1'b0), .LDPIPEEN(1'b0),
                .CINVCTRL(1'b0), .CNTVALUEIN(5'd0), .CNTVALUEOUT(),
                .DATAIN(1'b0), .REGRST(1'b0)
            );
            IDDR #(.DDR_CLK_EDGE("SAME_EDGE_PIPELINED"), .SRTYPE("SYNC")) u_iddr (
                .C(rxc_io), .CE(1'b1), .D(rx_dly[gi]), .R(1'b0), .S(1'b0),
                .Q1(rx_q1[gi]), .Q2(rx_q2[gi])
            );
        end
    endgenerate

    reg [3:0] lo = 0, hi = 0;
    reg       ctl_r = 0, ctl_f = 0;
    always @(posedge rx_clk) begin
        lo <= rx_q1[3:0]; ctl_r <= rx_q1[4];
        hi <= rx_q2[3:0]; ctl_f <= rx_q2[4];
    end

    // 速率同步到 rx_clk 域
    wire [1:0] speed_rx;
    sync_ff #(.W(2)) u_sync_spd (.clk(rx_clk), .d(speed), .q(speed_rx));
    wire nib_mode = (speed_rx != 2'd3);

    // ---------------- 前导 / SFD / 组字节 ----------------
    localparam F_IDLE = 2'd0, F_PRE = 2'd1, F_DATA = 2'd2, F_DROP = 2'd3;
    reg [1:0] fst = F_IDLE;
    reg       nib_ph = 1'b0;
    reg [3:0] nib_lo = 0;
    reg       err_acc = 1'b0;

    reg       b_valid;       // 组合：本拍得到一个帧字节
    reg [7:0] b_data;

    // 暂存一个字节以便标记 last
    reg       hb_v = 1'b0;
    reg [7:0] hb = 0;
    reg       fifo_we = 1'b0;
    reg [9:0] fifo_din = 0;

    wire dv = ctl_r;
    wire er = ctl_r ^ ctl_f;

    always @(*) begin
        b_valid = 1'b0;
        b_data  = {hi, lo};
        if (fst == F_DATA && dv) begin
            if (!nib_mode) b_valid = 1'b1;
            else if (nib_ph) begin b_valid = 1'b1; b_data = {lo, nib_lo}; end
        end
    end

    always @(posedge rx_clk) begin
        fifo_we <= 1'b0;
        if (!dv) begin
            // 帧结束：写出最后一个字节
            if (fst == F_DATA && hb_v) begin
                fifo_we  <= 1'b1;
                fifo_din <= {1'b1, err_acc, hb};
            end
            fst <= F_IDLE; hb_v <= 1'b0; err_acc <= 1'b0; nib_ph <= 1'b0;
        end else begin
            case (fst)
                F_IDLE: begin
                    if (!nib_mode) fst <= ({hi, lo} == 8'hD5) ? F_DATA : (({hi, lo} == 8'h55) ? F_PRE : F_DROP);
                    else           fst <= (lo == 4'h5) ? F_PRE : F_DROP;
                    nib_ph <= 1'b0;
                end
                F_PRE: begin
                    if (!nib_mode) begin
                        if ({hi, lo} == 8'hD5)      fst <= F_DATA;
                        else if ({hi, lo} != 8'h55) fst <= F_DROP;
                    end else begin
                        if (lo == 4'hD)      begin fst <= F_DATA; nib_ph <= 1'b0; end
                        else if (lo != 4'h5) fst <= F_DROP;
                    end
                end
                F_DATA: begin
                    if (er) err_acc <= 1'b1;
                    if (nib_mode) begin
                        nib_ph <= ~nib_ph;
                        if (!nib_ph) nib_lo <= lo;
                    end
                    if (b_valid) begin
                        if (hb_v) begin
                            fifo_we  <= 1'b1;
                            fifo_din <= {1'b0, 1'b0, hb};
                        end
                        hb   <= b_data;
                        hb_v <= 1'b1;
                    end
                end
                default: ;
            endcase
        end
    end

    wire rst_rx;
    rst_sync u_rst_rx (.clk(rx_clk), .arst(rst_eth), .rst(rst_rx));

    async_fifo #(.DW(10), .AW(6)) u_rx_fifo (
        .wr_clk(rx_clk), .wr_rst(rst_rx), .wr_en(fifo_we), .din(fifo_din), .full(),
        .rd_clk(clk_eth), .rd_rst(rst_eth), .rd_en(rx_rd_en), .dout(rx_dout), .empty(rx_empty)
    );

    // =====================================================================
    // 速率检测：rxc/8 方波同步到 clk_eth，65.536 µs 窗口内计上升沿
    // =====================================================================
    reg [2:0] rx_div = 0;
    always @(posedge rx_clk) rx_div <= rx_div + 1'b1;
    wire div_s;
    sync_ff u_sync_div (.clk(clk_eth), .d(rx_div[2]), .q(div_s));
    reg        div_d = 0;
    reg [12:0] win = 0;
    reg [10:0] edges = 0;
    reg [1:0]  spd_cand = 0;
    always @(posedge clk_eth) begin
        div_d <= div_s;
        if (rst_eth) begin
            win <= 0; edges <= 0; speed <= 2'd0; spd_cand <= 2'd0;
        end else begin
            win <= win + 1'b1;
            if (win == 13'h1FFF) begin
                // 1G≈1024，100M≈205，10M≈20；连续两个窗口一致才切换
                spd_cand <= (edges >= 600) ? 2'd3 : (edges >= 100) ? 2'd2 : (edges >= 10) ? 2'd1 : 2'd0;
                if (spd_cand == ((edges >= 600) ? 2'd3 : (edges >= 100) ? 2'd2 : (edges >= 10) ? 2'd1 : 2'd0))
                    speed <= spd_cand;
                edges <= 0;
            end else if (div_s && !div_d && edges != 11'h7FF) begin
                edges <= edges + 1'b1;
            end
        end
    end

    // =====================================================================
    // TX
    // =====================================================================
    reg [5:0] ph = 0;            // 半字节内的拍号
    reg       tnib = 1'b0;       // 0：下一次输出低半字节
    reg [7:0] tbyte = 0;
    reg       ten = 1'b0;
    reg [3:0] d1 = 0, d2 = 0;
    reg       c1 = 0, c2 = 0;    // TX_CTL 两沿
    reg       k1 = 1, k2 = 0;    // TXC 两沿

    wire [5:0] per   = (speed == 2'd1) ? 6'd50 : 6'd5;   // 半字节周期
    wire [5:0] upd   = (speed == 2'd1) ? 6'd37 : 6'd3;   // 数据更新拍（TXC 低电平中段）
    wire [5:0] half  = (speed == 2'd1) ? 6'd25 : 6'd2;

    always @(posedge clk_eth) begin
        byte_en <= 1'b0;
        if (rst_eth) begin
            ph <= 0; tnib <= 1'b0; ten <= 1'b0;
            d1 <= 0; d2 <= 0; c1 <= 0; c2 <= 0; k1 <= 1'b1; k2 <= 1'b0;
        end else if (speed == 2'd3 || speed == 2'd0) begin
            // 千兆（无时钟时也按千兆空转）
            byte_en <= 1'b1;
            d1 <= tx_data[3:0]; d2 <= tx_data[7:4];
            c1 <= tx_en;        c2 <= tx_en;
            k1 <= 1'b1;         k2 <= 1'b0;
        end else begin
            ph <= (ph == per - 1'b1) ? 6'd0 : ph + 1'b1;
            // TXC：前半周期高，后半周期低（百兆 2.5 拍用 ODDR 半拍实现）
            if (ph < half)       begin k1 <= 1'b1; k2 <= 1'b1; end
            else if (ph == half) begin k1 <= 1'b1; k2 <= (speed == 2'd1); end
            else                 begin k1 <= 1'b0; k2 <= 1'b0; end
            if (ph == upd) begin
                if (!tnib) begin
                    tbyte   <= tx_data;
                    ten     <= tx_en;
                    byte_en <= 1'b1;
                    d1 <= tx_data[3:0]; d2 <= tx_data[3:0];
                    c1 <= tx_en;        c2 <= tx_en;
                end else begin
                    d1 <= tbyte[7:4];   d2 <= tbyte[7:4];
                    c1 <= ten;          c2 <= ten;
                end
                tnib <= ~tnib;
            end
        end
    end

    // byte_en 是寄存器输出，消费的是上一拍的 tx_data：引擎在 byte_en 拍之后才前进。
    // 千兆模式下 byte_en 恒为 1，每拍一个字节。

    generate
        for (gi = 0; gi < 4; gi = gi + 1) begin : g_tx
            ODDR #(.DDR_CLK_EDGE("SAME_EDGE"), .SRTYPE("SYNC")) u_oddr_d (
                .C(clk_eth), .CE(1'b1), .D1(d1[gi]), .D2(d2[gi]), .R(1'b0), .S(1'b0), .Q(rgmii_txd[gi])
            );
        end
    endgenerate
    ODDR #(.DDR_CLK_EDGE("SAME_EDGE"), .SRTYPE("SYNC")) u_oddr_ctl (
        .C(clk_eth), .CE(1'b1), .D1(c1), .D2(c2), .R(1'b0), .S(1'b0), .Q(rgmii_tx_ctl)
    );

    // TXC_90：图样先在 90° 时钟域重打一拍（clk_eth→clk_eth90 只有 2 ns 余量，
    // 路径是相邻触发器），再由 90° 时钟驱动 ODDR，TXC 比数据晚 2 ns。
    wire txc_clk, k1_t, k2_t;
    generate
        if (TXC_90) begin : g_txc90
            reg r1 = 1'b1, r2 = 1'b0;
            always @(posedge clk_eth90) begin r1 <= k1; r2 <= k2; end
            assign txc_clk = clk_eth90;
            assign k1_t = r1;
            assign k2_t = r2;
        end else begin : g_txc0
            assign txc_clk = clk_eth;
            assign k1_t = k1;
            assign k2_t = k2;
        end
    endgenerate
    ODDR #(.DDR_CLK_EDGE("SAME_EDGE"), .SRTYPE("SYNC")) u_oddr_txc (
        .C(txc_clk), .CE(1'b1), .D1(k1_t), .D2(k2_t), .R(1'b0), .S(1'b0), .Q(rgmii_txc)
    );
endmodule
