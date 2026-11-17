# FPGA ↔ MCU 通信协议

按《核脉冲多道分析器顶层设计》第 5 节实现，本文补全了规划里没写死的细节（寄存器表、波形包、状态位、
SET_NET 下标等）。实现以 `fpga/rtl/mca_regs.vh`、`tools/mcaproto.py`、`mcu/app/mca_proto.h` 为准，三者一致。

- 全部字段**大端**；每个包以 4 字节 ASCII magic 开头。
- 时间单位：ADC 采样拍，32 MHz，1 拍 = 31.25 ns。
- FPGA 默认 IP 192.168.1.10、MAC 02:00:4D:43:41:01；MCU 默认 192.168.1.20。

| 端口 | 方向 | 包 |
|------|------|----|
| UDP 5000 | 主机 → FPGA | MCCM 命令 |
| UDP 5000 | FPGA → 主机 | MCRS 应答、MCST 心跳（1 Hz） |
| UDP 5001 | FPGA → 主机 | MCEV 事件流、MCWF 波形 |
| UDP 6000 | PC → MCU | MCU 文本控制台（本工程扩展，见文末） |

**主机学习**：FPGA 把最近一个合法 MCCM 的发送方（MAC/IP/端口无关，事件和心跳都发到对方的 5000/5001）记为主机，
收到第一个 MCCM 之前不发心跳和事件（事件先留在 1024 深的 FIFO 里，满了才丢弃并计入 `dropped_events`）。`SET_NET` 置 `HOST_LOCK=1` 后不再改主机。
所以同一时刻只能有一个主机：MCU 在跑时用 PC 的 `mca_tool.py` 直接发命令，会把事件流抢到 PC。

## MCCM 命令（16 字节）

| 偏移 | 长度 | 字段 |
|------|------|------|
| 0 | 4 | "MCCM" |
| 4 | 1 | cmd |
| 5 | 1 | seq（应答原样带回） |
| 6 | 2 | 保留 0 |
| 8 | 4 | addr |
| 12 | 4 | value |

| cmd | 名称 | addr | value | 应答 value |
|-----|------|------|-------|------------|
| 0x01 | PING | – | – | 版本 0x00010000 |
| 0x02 | WRITE_REG | 寄存器地址 | 值 | 写入值 |
| 0x03 | READ_REG | 寄存器地址 | – | 读出值 |
| 0x04 | CLEAR_COUNTERS | – | – | – |
| 0x05 | SET_STREAM | 每包事件数 1..64 | 超时 1..10000 ms | – |
| 0x06 | CAPTURE_WAVE | 见下 | – | – |
| 0x07 | GET_STATUS | – | – | status_flags |
| 0x08 | RESET | – | – | 配置恢复默认、计数清零、停止 |
| 0x09 | SET_NET | 下标 0..7 | 值 | 值（MCRS 发出后才生效） |

CAPTURE_WAVE 的 addr：`[31:24]` 信号源（0 原始 ADC、1 极性校正后、2 快滤波、3 慢滤波；bit7 = 1 不等触发立即捕获），
`[23:16]` 深度码 0..4（256 << 码，即 256..4096 点），`[15:0]` 触发前点数（< 深度）。捕获正在发送时应答 BUSY；
已布防未触发时再发会重新布防。

SET_NET 下标：0 本机 IP，1 本机 MAC 高 16 位，2 本机 MAC 低 32 位，3 主机 IP，4 主机 MAC 高 16 位，
5 主机 MAC 低 32 位，6 HOST_VALID，7 HOST_LOCK。

## MCRS 应答（16 字节）

| 偏移 | 长度 | 字段 |
|------|------|------|
| 0 | 4 | "MCRS" |
| 4 | 1 | cmd |
| 5 | 1 | seq |
| 6 | 1 | status：0 OK、1 BAD_CMD、2 BAD_ADDR、3 BAD_VALUE、4 BUSY |
| 7 | 1 | 保留 |
| 8 | 4 | value |
| 12 | 4 | 保留 |

应答发往该 MCCM 的源 IP:端口。MCU 侧超时 100 ms 重发（序号加 1），3 次失败记为 TIMEOUT（0xFF，仅 MCU 内部用）。

## MCEV 事件流（16 + 8n 字节）

| 偏移 | 长度 | 字段 |
|------|------|------|
| 0 | 4 | "MCEV" |
| 4 | 1 | 版本 1 |
| 5 | 1 | 保留 |
| 6 | 2 | n，事件数 1..64 |
| 8 | 4 | seq，包序号（每包加 1，CLEAR_COUNTERS 不清） |
| 12 | 2 | ts_epoch，时间戳高 16 位 |
| 14 | 2 | 保留 |
| 16 | 8n | 事件记录 |

事件记录：`ts_low u32 | energy u16 | flags u8 | 保留 u8`。完整时间戳 = `ts_epoch << 32 | ts_low`（48 位），
同一包内 ts_epoch 相同（跨 epoch 时提前发包）。

flags：bit0 ADC 饱和，bit1 基线越界，bit2 测试源事件，bit3 堆积。

时间戳是快滤波越过阈值的拍（前沿），随脉冲幅度有几拍的走动，不是峰位时刻。

能量：`E = ((H >>> S) × G) >> 14`，H 为梯形平顶高度；`filter_params()` 选 G、S 使 ADC 满幅 255 LSB 对应 65280。
MCU 按 `道址 = E >> (16 − log2 道数)` 进谱。

发包条件：攒满 n 个、距本包第一个事件超过 SET_STREAM 的超时、或 ts_epoch 变化。

## MCST 心跳（64 字节，1 Hz）

| 偏移 | 长度 | 字段 |
|------|------|------|
| 0 | 4 | "MCST" |
| 4 | 1 | 版本 1 |
| 5 | 3 | 保留 |
| 8 | 4 | seq，心跳序号（从 0 起） |
| 12 | 4 | pkt_seq，下一个 MCEV 的序号 |
| 16 | 8 | total_events，触发总数（含堆积） |
| 24 | 4 | pileup_events |
| 28 | 4 | dropped_events，事件 FIFO 满或尚无主机而丢弃 |
| 32 | 8 | total_ticks，RUN 期间的拍数（实时间） |
| 40 | 8 | busy_ticks，死时间拍数 |
| 48 | 4 | adc_sat_events |
| 52 | 4 | status_flags |
| 56 | 8 | 保留 |

**busy_ticks 的定义**：每个触发 t 周围 [t−W, t+W]（W = PILEUP_WIN）的并集。这段时间里出现的事件都会被标为堆积，
所以 `live = total_ticks − busy_ticks` 正好是"能产生干净事件"的时间，干净计数 / 活时间就是无偏的真实计数率。
INHIBIT（基线越界等）期间也计入 busy。

status_flags（同 STATUS 寄存器、GET_STATUS 应答）：

| 位 | 含义 |
|----|------|
| 0 | RUN |
| 1 | 链路通 |
| 3:2 | 速率 0 无、1 10M、2 100M、3 1000M |
| 4 | 已有主机 |
| 5 | 测试源输入 |
| 6 | 波形捕获中 |
| 7 | 事件 FIFO 溢出过 |
| 8 | ADC 饱和过 |
| 9 | 基线越界 |
| 10 | 堆积事件丢弃（CTRL.PU_DISCARD） |
| 11 | INHIBIT |
| 12 | HOST_LOCK |

## MCWF 波形（24 + 2n 字节，UDP 5001）

| 偏移 | 长度 | 字段 |
|------|------|------|
| 0 | 4 | "MCWF" |
| 4 | 1 | 版本 1 |
| 5 | 1 | 信号源（同 CAPTURE_WAVE） |
| 6 | 2 | n，本包点数 ≤ 256 |
| 8 | 4 | capture_id，每次捕获加 1 |
| 12 | 2 | offset，本包第一个点的下标 |
| 14 | 2 | total，总点数 |
| 16 | 4 | trig_ts，触发时间戳低 32 位 |
| 20 | 2 | pretrig |
| 22 | 2 | 保留 |
| 24 | 2n | int16 采样 |

## 寄存器表（字节地址）

配置寄存器（读写，超出范围的写应答 BAD_VALUE）：

| 地址 | 名称 | 说明 |
|------|------|------|
| 0x00 | ID | 只读 "MCA1" = 0x4D434131 |
| 0x04 | VERSION | 只读 0x00010000 |
| 0x08 | CTRL | bit0 RUN，bit1 SRC_TEST（测试源代替 ADC），bit2 POL_NEG，bit3 PU_DISCARD，bit4 PEAK_FIXED |
| 0x0C | STATUS | 只读 status_flags |
| 0x10 | FAST_K | 快滤波长度 1..63 |
| 0x14 | FAST_GAP | 快滤波间隔 0..63 |
| 0x18 | THRESHOLD | 触发阈值，快滤波输出单位（= LSB × FAST_K）1..65535 |
| 0x20 | SLOW_K | 梯形上升 k 1..1023 |
| 0x24 | SLOW_M | 梯形平顶 m 0..255 |
| 0x28 | PZ_M | 极零参数 M = 1/(e^(Ts/τ) − 1)，Q16.8 |
| 0x2C | PEAK_DELAY | 峰值窗起点（相对触发） |
| 0x30 | PEAK_WIN | 峰值窗长度 1..4095 |
| 0x34 | BLR_SHIFT | 基线恢复时间常数 2^n 拍 |
| 0x38 | PILEUP_WIN | 堆积判别窗 W 1..4095 |
| 0x3C | BLR_HOLD | 触发后基线恢复冻结拍数 |
| 0x40 | ENERGY_GAIN | G |
| 0x44 | ENERGY_SHIFT | S 0..47 |
| 0x48 | BL_WIN | 输入基线允许范围 [7:0] 下限、[15:8] 上限 |
| 0x4C | TOT_MAX | 过阈时间上限，超过记堆积（0 关闭） |
| 0x50 | TP_RATE | 测试源每拍发脉冲概率 × 2^32 |
| 0x54 | TP_AMP | 测试源幅度，8.8 定点 LSB |
| 0x58 | TP_CFG | [1:0] 模式（0 定幅、1 峰 + 平台、2 均匀），[7:4] 噪声，[11:8] 衰减移位，[15:12] 上升移位 |
| 0x5C | TP_BASE | 测试源基线 |

写滤波参数后 FPGA 自动冲刷滤波器（约 130 µs），期间不出事件。物理量到寄存器的换算见 `mcaproto.filter_params()` /
`mcu/app/filter_cfg.c`（两者有单元测试对照）。

只读计数与状态：

| 地址 | 名称 |
|------|------|
| 0x100/0x104 | TOTAL_EVENTS 低/高 32 位 |
| 0x108 | PILEUP_EVENTS |
| 0x10C | DROPPED_EVENTS |
| 0x110/0x114 | TOTAL_TICKS |
| 0x118/0x11C | BUSY_TICKS |
| 0x120 | ADC_SAT_EVENTS |
| 0x124 | BASELINE（8.8 LSB） |
| 0x128/0x12C | TIMESTAMP（当前拍） |
| 0x130 | PKT_SEQ |
| 0x134 | NET_STATUS：[1:0] 速率，[3] 链路，[4] HOST_VALID，[5] HOST_LOCK |
| 0x138 | STREAM_N |
| 0x13C | STREAM_MS |
| 0x140 | LOCAL_IP |
| 0x144 | HOST_IP |
| 0x148 | RX_FRAMES，收到的帧数 |
| 0x14C | RX_ERRORS，CRC 错帧数（RGMII 时序不对时会涨） |

## MCU 文本控制台（UDP 6000）

一个数据报里可以有多行命令；回复发回来源地址，最后一行以 `OK` 或 `ERR ...` 开头，长回复分成多个 ≤ 1400 字节的
数据报（行不跨包）。`help` 列出全部命令，常用的：

```
status                      采集状态、实/活时间、计数率、丢包与补偿系数
start [live_s] | stop | clear
get <key> | set <key> <v> | dump | save | defaults
spec [first [count]]        SPEC first count nch total，随后每行 16 道
peaks                       寻峰拟合结果
cal <ch> <E> [...] | calpk <E1> [E2 ...] | cal off
wave <src> [depth [pre [force]]]
store <slot> [name] | recall <slot> | slots
reg <addr> [value]          透传读写 FPGA 寄存器
```

PC 端：`python tools/mca_tool.py --mcu 192.168.1.20 mcu status`，`... mcu-spec --out spec.csv`。
