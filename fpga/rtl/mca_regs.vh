// 寄存器表（字节地址 = 索引 × 4）。eth_core 与 adc_domain 共用。
// 0x000-0x05C：配置寄存器（READ_REG / WRITE_REG）
localparam R_ID           = 0;   // RO "MCA1"
localparam R_VERSION      = 1;   // RO
localparam R_CTRL         = 2;   // bit0 RUN, bit1 SRC_TEST, bit2 POL_NEG, bit3 PU_DISCARD, bit4 PEAK_FIXED
localparam R_STATUS       = 3;   // RO status_flags
localparam R_FAST_K       = 4;   // 快滤波长度 1..63
localparam R_FAST_GAP     = 5;   // 快滤波间隔 0..63
localparam R_THRESHOLD    = 6;   // 触发阈值（快滤波单位 = LSB × FAST_K）1..65535
localparam R_SLOW_K       = 8;   // 梯形上升 k 1..1023
localparam R_SLOW_M       = 9;   // 梯形平顶 m 0..255
localparam R_PZ_M         = 10;  // 极零参数 M，Q16.8
localparam R_PEAK_DELAY   = 11;  // 峰值窗起点（相对触发拍）
localparam R_PEAK_WIN     = 12;  // 峰值窗长度 1..4095
localparam R_BLR_SHIFT    = 13;  // BLR 时间常数 2^n 拍 0..15
localparam R_PILEUP_WIN   = 14;  // 堆积判别窗 W 1..4095
localparam R_BLR_HOLD     = 15;  // 触发后 BLR 冻结拍数
localparam R_ENERGY_GAIN  = 16;  // 能量增益 G
localparam R_ENERGY_SHIFT = 17;  // 能量移位 S 0..47：E = ((H >>> S) × G) >> 14
localparam R_BL_WIN       = 18;  // 输入基线允许范围 [7:0] 下限 [15:8] 上限
localparam R_TOT_MAX      = 19;  // 过阈时间上限（0 关闭）
localparam R_TP_RATE      = 20;  // 测试源每拍概率 × 2^32
localparam R_TP_AMP       = 21;  // 测试源幅度 8.8 LSB
localparam R_TP_CFG       = 22;  // [1:0] 模式 [7:4] 噪声 [11:8] 衰减移位 [15:12] 上升移位
localparam R_TP_BASE      = 23;  // 测试源基线
localparam NCFG           = 24;

localparam [31:0] MCA_ID      = 32'h4D43_4131;   // "MCA1"
localparam [31:0] MCA_VERSION = 32'h0001_0000;   // 1.0.0

// 0x100-0x14C：只读计数/状态
localparam A_TOTAL_EVENTS_LO = 16'h100;
localparam A_TOTAL_EVENTS_HI = 16'h104;
localparam A_PILEUP_EVENTS   = 16'h108;
localparam A_DROPPED_EVENTS  = 16'h10C;
localparam A_TOTAL_TICKS_LO  = 16'h110;
localparam A_TOTAL_TICKS_HI  = 16'h114;
localparam A_BUSY_TICKS_LO   = 16'h118;
localparam A_BUSY_TICKS_HI   = 16'h11C;
localparam A_ADC_SAT_EVENTS  = 16'h120;
localparam A_BASELINE        = 16'h124;
localparam A_TIMESTAMP_LO    = 16'h128;
localparam A_TIMESTAMP_HI    = 16'h12C;
localparam A_PKT_SEQ         = 16'h130;
localparam A_NET_STATUS      = 16'h134;
localparam A_STREAM_N        = 16'h138;
localparam A_STREAM_MS       = 16'h13C;
localparam A_LOCAL_IP        = 16'h140;
localparam A_HOST_IP         = 16'h144;
localparam A_RX_FRAMES       = 16'h148;
localparam A_RX_ERRORS       = 16'h14C;

// ADC 域操作码（eth → adc 握手通道）
localparam [2:0] OP_CFG = 3'd1, OP_SNAP = 3'd2, OP_CLEAR = 3'd3, OP_RESET = 3'd4, OP_ARM = 3'd5;

function [31:0] cfg_default;
    input integer i;
    case (i)
        R_FAST_K:       cfg_default = 8;
        R_FAST_GAP:     cfg_default = 8;
        R_THRESHOLD:    cfg_default = 64;
        R_SLOW_K:       cfg_default = 64;
        R_SLOW_M:       cfg_default = 32;
        R_PZ_M:         cfg_default = 1023 * 256;   // 与测试源 decay=10 匹配
        R_PEAK_DELAY:   cfg_default = 63;           // k + 7 - kf
        R_PEAK_WIN:     cfg_default = 42;           // m + kf + 2
        R_BLR_SHIFT:    cfg_default = 9;
        R_PILEUP_WIN:   cfg_default = 112;          // k + m + kf + 8
        R_BLR_HOLD:     cfg_default = 184;          // 2k + m + kf + 16
        R_ENERGY_GAIN:  cfg_default = 32768;        // 满幅 255 LSB → 65280
        R_ENERGY_SHIFT: cfg_default = 17;
        R_BL_WIN:       cfg_default = 32'h0000_FF00;
        R_TP_RATE:      cfg_default = 671089;       // 5 kcps
        R_TP_AMP:       cfg_default = 32'h6400;     // 100 LSB
        R_TP_CFG:       cfg_default = 32'h3A11;     // 峰+平台，噪声 1，decay 10，rise 3
        R_TP_BASE:      cfg_default = 16;
        default:        cfg_default = 0;
    endcase
endfunction

// 可写位掩码；0 表示该地址不可写
function [31:0] cfg_mask;
    input integer i;
    case (i)
        R_CTRL:         cfg_mask = 32'h1F;
        R_FAST_K:       cfg_mask = 32'h3F;
        R_FAST_GAP:     cfg_mask = 32'h3F;
        R_THRESHOLD:    cfg_mask = 32'hFFFF;
        R_SLOW_K:       cfg_mask = 32'h3FF;
        R_SLOW_M:       cfg_mask = 32'hFF;
        R_PZ_M:         cfg_mask = 32'hFF_FFFF;
        R_PEAK_DELAY:   cfg_mask = 32'hFFF;
        R_PEAK_WIN:     cfg_mask = 32'hFFF;
        R_BLR_SHIFT:    cfg_mask = 32'hF;
        R_PILEUP_WIN:   cfg_mask = 32'hFFF;
        R_BLR_HOLD:     cfg_mask = 32'hFFF;
        R_ENERGY_GAIN:  cfg_mask = 32'hFFFF;
        R_ENERGY_SHIFT: cfg_mask = 32'h3F;
        R_BL_WIN:       cfg_mask = 32'hFFFF;
        R_TOT_MAX:      cfg_mask = 32'hFFF;
        R_TP_RATE:      cfg_mask = 32'hFFFF_FFFF;
        R_TP_AMP:       cfg_mask = 32'hFFFF;
        R_TP_CFG:       cfg_mask = 32'hFFFF;
        R_TP_BASE:      cfg_mask = 32'hFF;
        default:        cfg_mask = 32'h0;
    endcase
endfunction

// 取值下限 / 上限
function [31:0] cfg_min;
    input integer i;
    case (i)
        R_FAST_K, R_THRESHOLD, R_SLOW_K, R_PEAK_WIN, R_PILEUP_WIN: cfg_min = 1;
        default: cfg_min = 0;
    endcase
endfunction

function [31:0] cfg_max;
    input integer i;
    case (i)
        R_ENERGY_SHIFT: cfg_max = 47;
        default:        cfg_max = cfg_mask(i);
    endcase
endfunction
