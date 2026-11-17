# 核脉冲多道分析器（MCA）

按《核脉冲多道分析器顶层设计》实现的 FPGA 前端 + MCU 终端。

```
探测器/前放 → AD9280 (8 bit, 32 MSPS) → Artix-7 xc7a35t（正点原子达芬奇Pro）
              基线恢复、极零、梯形成形、峰值、堆积判别、死时间计账、事件流
                         │ UDP（MCEV 事件 / MCST 心跳 / MCCM-MCRS 命令 / MCWF 波形）
                         ▼
             STM32H743VIT6 + LAN8720 + ST7796S 4.0" 480×320（或笔记本上的 mca_host，见下）
              能量刻度、直方图、死时间/丢包校正、寻峰拟合、显示、存储、文本控制台
```

协议细节见 [docs/protocol.md](docs/protocol.md)，FPGA 顶层 Block Design（Vivado 导出）见 [docs/mca_block.pdf](docs/mca_block.pdf)。

## 目录

| 路径 | 内容 |
|------|------|
| `fpga/rtl` | Verilog 模块：`adc_if`、`pulse_proc`（滤波与判别）、`wave_capture`、`rgmii_if`、`eth_*`（硬件 UDP/ARP）、`mcev_packer`、`test_pulser`、`phy_rst`、`led_drv` |
| `fpga/constr` | 达芬奇Pro 引脚与时序约束 |
| `fpga/sim` | xsim 测试台：`tb_pulse`（滤波链）、`tb_top`（整机，含 PHY 与 AD9280 模型） |
| `fpga/scripts/block_design.tcl` | 顶层 Block Design，生成 Vivado 工程 |
| `fpga/scripts/build.tcl` | 全流程：建工程 → 综合 → 实现 → 位流 |
| `mcu/app` | 与硬件无关的 MCU 应用（协议、FPGA 链路、能谱、寻峰、界面、控制台…） |
| `mcu/port/stm32h7` | STM32H743 固件：时钟/MPU、ST7796S、以太网 + LwIP、闪存存储 |
| `mcu/port/host` | PC 宿主版：同一套 `app/` 跑在 Windows / Linux（WSL）上，真实 UDP + 窗口显示；笔记本代替 STM32 时用它 |
| `mcu/tests` | 单元测试、宿主版 + 假 FPGA 端到端测试 |
| `tools/mca_tool.py` | PC 工具：直连 FPGA 测试、假 FPGA、MCU 控制台客户端 |

## 拿到代码之后

```
git clone https://github.com/pingg241/mcn.git
```

都要 Git、Python 3 和 [xmake](https://xmake.io)，其余按要做的事装：

| 要做的事 | 还需要 | 第三方库 |
|----------|--------|----------|
| 烧 FPGA | Vivado 的 Hardware Manager（只烧不编译可以装小得多的 Vivado Lab Edition）；编好的位流已在仓库里 | 不用 |
| 笔记本终端 mca_host、单元测试 | Windows：MSYS2 里 `pacman -S mingw-w64-x86_64-gcc`；Linux：`gcc`、`libx11-dev` | 不用 |
| STM32 固件 | Windows：MSYS2 里 `pacman -S mingw-w64-x86_64-arm-none-eabi-gcc` | 要 |
| 改 FPGA 后重新编译、仿真 | Vivado 2023.2 | 不用 |

`mca_tool.py` 的 `--plot` 画图另外要 `pip install matplotlib`。

第三方库（ST 的 HAL、CMSIS 和 LwIP，约 80 MB）不放在仓库里，只有编 STM32 固件才要。在项目根目录运行一次：

```
python mcu/scripts/fetch_deps.py
```

它用 git 从 ST 的 GitHub 按脚本里写死的版本浅克隆到 `mcu/third_party/`，十几秒。版本固定，谁拉下来编出的固件都一模一样。

编译命令见下面"MCU"一节，有两个参数按自己的机器填：

- `--mingw=`：MSYS2 不在默认的 `C:\msys64` 时，给 `xmake f -p mingw` 加 `--mingw=<MSYS2 目录>/mingw64`。
- `--sdk=`：编固件用，填 arm-none-eabi-gcc 所在 `bin` 目录的上一级，MSYS2 装的就是 `<MSYS2 目录>/mingw64`。
  Linux 上 `sudo apt install gcc-arm-none-eabi` 后用 `--sdk=/usr`（这条没实测过）。

编出来的 `mca_host.exe` 只依赖 Windows 自带的 DLL，拷到没装 MSYS2 的电脑上也能直接运行。

## FPGA

顶层是 Vivado Block Design `mca_block`，由 `scripts/block_design.tcl` 生成：各方块以 Module Reference 方式直接引用
`rtl/` 的源码，复位用 Xilinx 的 Processor System Reset。这个脚本就是顶层的源码，工程每次都重新生成——在 GUI 里改了图，
要把改动写回脚本，否则下次编译会被覆盖。

Vivado 2023.2（在 `fpga/` 下运行）：

```
vivado -mode batch -nojournal -log build/vivado.log -source scripts/build.tcl
```

它先运行 `block_design.tcl` 建工程 `build/mca_block/mca_block.xpr`，再综合、实现、出位流；跑完可以用 Vivado 打开这个工程
看图和结果，也可以在 GUI 里直接改、重新编译。只建工程不编译：`vivado -mode batch -nojournal -nolog -source scripts/block_design.tcl`。

产物 `fpga/build/mca.bit`（JTAG 下载）和 `mca.bin`（SPIx4 配置闪存，Hardware Manager 里
Add Configuration Memory Device 后烧写）。这两个文件随仓库提交，和当前源码一致；改了 RTL 重新编译后记得一起提交。当前结果：WNS +0.221 ns、WHS +0.036 ns；LUT 4183（20%）、
FF 5749、BRAM 5.5、DSP 7。

仿真（Vivado 自带 xsim）：

```
python fpga/sim/run_sim.py tb_pulse
python fpga/sim/run_sim.py tb_top --giga 1     # 千兆；--giga 0 百兆
```

`tb_top` 走完整链路：PHY 模型收发以太网帧，`tb_top_frames.py` 检查 ARP、命令应答、事件能量（误差 ≤ 0.5%）、
心跳与波形包。仿真的就是顶层 Block Design：第一次运行或改了 `block_design.tcl`、`rtl/` 之后，会先用 Vivado 生成仿真版工程
`build/mca_sim/`（心跳 1 ms、PHY 复位 100 拍，其余和上板的一样），约半分钟。

不接 MCU 时可以用 PC 直接测板子（网口直连 PC，PC 设 192.168.1.x）：

```
python tools/mca_tool.py ping
python tools/mca_tool.py setup --tau 32 --rise 2 --flat 1 --thr 8 --test   # 片内测试源（τ = 32 µs）；接探测器去掉 --test、--tau 填前放的值
python tools/mca_tool.py run --time 60 --out spec.csv --plot
python tools/mca_tool.py wave --src 1 --plot
```

## MCU

工具链和第三方库见上面"拿到代码之后"。在 `mcu/` 下：

```
# PC 宿主版与单元测试（Windows；MSYS2 不在 C:\msys64 时加 --mingw=<MSYS2 目录>/mingw64）
xmake f -p mingw && xmake
xmake run tests
python tests/e2e_host.py          # 假 FPGA + 宿主版，结果和截图在 build/e2e/

# 同上（Linux / WSL：gcc + libx11-dev，没有 libX11 就编成无窗口版）
xmake f -p linux && xmake
xmake run tests
python3 tests/e2e_host.py

# STM32H743 固件（先 fetch_deps；--sdk 按自己的 MSYS2 目录改）
xmake f -p cross -a cortex-m7 --toolchain=gnu-rm --sdk=D:/msys64/mingw64 && xmake
```

固件在 `build/cross/cortex-m7/release/firmware.{elf,hex,bin}`，从 0x08000000 烧写（STM32CubeProgrammer、
ST-Link 或 USB DFU 均可）。Flash 用 116 KB；RAM：AXI SRAM 放 .bss（含 300 KB 帧缓冲）与堆，DTCM 放栈，
SRAM1 放寻峰工作区，SRAM2 放以太网描述符、缓冲和 LwIP 堆（MPU 不可缓存）。

宿主版配假 FPGA 试运行：

```
python tools/mca_tool.py fake-fpga --ip 127.0.0.2 --rate 20000 --loss 0.01
mcu/build/mingw/x86_64/release/mca_host.exe --fpga 127.0.0.2 --local 127.0.0.1
```

两个都开着不要关。默认是片内测试源（能谱上只有一根线），在 mca_host 的命令行窗口里输入 `set test_src 0`
切到模拟的 NaI 谱。

（Linux 上可执行文件是 `mcu/build/linux/x86_64/release/mca_host`。`--local 127.0.0.1` 不能省：Linux 不允许在假 FPGA
已占的 `127.0.0.2:5000` 之上再绑 `0.0.0.0:5000`。）`--no-window` 无窗口运行，配合 `--seconds N --bmp 前缀`
在退出时把三页存成 BMP（e2e 测试就这样用）。

### 笔记本作终端（代替 STM32）

`mca_host.exe` 跑的是和固件同一套 `app/` 代码，FPGA 侧和协议都不用改（FPGA 只认最后给它发命令的主机）。

1. 网线把笔记本网口（没有就用 USB 千兆网卡）直连达芬奇Pro 的网口。该网卡设静态 IP：`192.168.1.20`，
   掩码 `255.255.255.0`，网关留空（FPGA 默认 `192.168.1.10`）。
2. 运行 `mcu\build\mingw\x86_64\release\mca_host.exe`；FPGA 不是默认地址时加 `--fpga IP`。
3. 第一次运行 Windows 防火墙会弹窗，**"公用网络"也要勾上**：直连的网线通常被识别成"未识别的网络"（公用），
   不放行就收不到 FPGA 的包，状态栏一直是 `NO FPGA`。
4. 窗口按屏幕自动取最大整数倍，可以拉伸、最大化。空格 / 回车 / 鼠标左键 = 板上的 KEY：短按切页，
   长按 1 s 清谱（波形页换信号源）。
5. 命令行窗口里显示日志，也直接在里面输入控制台命令（见下文"控制台"）；存谱写在当前目录的 `host_nv/`
   （`--nv` 可改）。

mca_host 开着时不要再用 `mca_tool.py ping/setup/run/wave` 直连 FPGA：端口被占用，FPGA 也会把事件流转给后来者。

**在 WSL / Linux 里跑**：在 `mcu/` 下 `xmake f -p linux && xmake`，运行 `build/linux/x86_64/release/mca_host`。
窗口由 WSLg 显示成普通的 Windows 窗口，按键同上。网络上要注意：

- WSL 必须是镜像网络（`~/.wslconfig` 里 `networkingMode=mirrored`）。NAT 模式下 FPGA 看到的是 Windows 的地址，
  事件包发不进 WSL。镜像模式下，笔记本网卡的 `192.168.1.20` 会原样出现在 WSL 里，上面第 1 步照做即可。
- 第 3 步的防火墙弹窗只管 Windows 程序。WSL 的入站由 Hyper-V 防火墙管，默认规则是拦截；状态栏一直是 `NO FPGA` 时，
  在管理员 PowerShell 里放行一次：

  ```
  New-NetFirewallHyperVRule -Name MCA-UDP -DisplayName "MCA UDP" -Direction Inbound -VMCreatorId '{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}' -Protocol UDP -LocalPorts 5000,5001,6000 -Action Allow
  ```

- 插网线、改 IP 之后 WSL 里看不到新地址（`ip -4 addr`），就 `wsl --shutdown` 再开。
- Windows 版和 WSL 版不要同时开：两边共用端口，FPGA 也只认一个主机。
- 启动时提示 "UDP 5001 接收缓冲只有 … KB"（非 root 用户）就按提示调大 `net.core.rmem_max`，否则高计数率时容易丢包。

### 屏幕与按键

三页：能谱（对数/线性、峰标记、刻度后横轴为能量）、波形（自动抓取，长按换信号源）、统计
（实/活时间、ICR/OCR、活时间校正计数率、丢包与补偿、配置、峰表）。KEY 短按切页，长按清谱。

### 控制台

文本命令，一行一条，`help` 列出全部。笔记本上直接在 mca_host 的命令行窗口里输入（`quit` 退出）：

```
status
set tau_us 50
peaks
cal 1240.3 661.657 2497.4 1332.492     # 两点刻度：道址 能量 道址 能量，道址从 peaks 里抄
store 1 cs137                          # 存谱到槽 1；recall 1 调出，start 继续采集
save                                   # 保存设置
```

`calpk 661.657 1332.492` 会把面积最大的几个峰按道址顺序对上给出的能量，谱里反散射峰比较大时会对错，优先用 `cal`。

STM32 板子没有键盘，同样的命令走 UDP 6000，用 `mca_tool.py` 发；笔记本上要从脚本里调用也可以这样（`--mcu 127.0.0.1`）：

```
python tools/mca_tool.py --mcu 192.168.1.20 mcu status
python tools/mca_tool.py --mcu 192.168.1.20 mcu-spec --out spec.csv
```

`set` 改滤波参数会自动重新下发 FPGA 并清谱；`save` 才写入闪存（擦扇区约 1~2 s，期间丢的事件包按丢包补偿）。

## 上板前要核对的假设

这些在仿真里无法验证，按需改参数：

| 项 | 位置 | 默认 | 现象 / 调整 |
|----|------|------|-------------|
| RGMII 接收延时 | `block_design.tcl` 里 `u_rgmii` 的 `IDELAY_VALUE` | 10 抽头（0.78 ns） | `reg 0x14C`（RX_ERRORS）持续增长就调 |
| RGMII 发送时钟相位 | `block_design.tcl` 里 `u_rgmii` 的 `TXC_90` | 1（TXC 移 90°，参考已上板验证的工程） | PC 抓不到 FPGA 的包时改 0 |
| AD9280 采样相位 | `block_design.tcl` 里 `u_clk` 的 `ADC_CLK_PHASE` | 180° | 波形出现毛刺/台阶时调整 |
| PHY 型号 | 达芬奇Pro 板载 YT8511 | 上电自协商 | `status` 里看 link 与速率 |
| MCU 板 | `mcu/port/stm32h7/board.h` | WeAct MiniSTM32H743：25 MHz HSE、LED PE3、KEY PC13 | 换板改这里 |
| 屏接线 | `board.h` | SPI4：SCK PE12、MOSI PE14、CS PE11、DC PE13、BL PE10、RST PE9 | 方向不对改 `LCD_MADCTL`（0x28/0xE8），颜色反了改 `LCD_INVERT`（默认 1，按 IPS 屏开反显） |
| 屏 SPI 速率 | `board.h` `LCD_SPI_PRESCALER` | 40 MHz | 花屏改 `_4`（20 MHz） |
| LAN8720 接线 | `board.h` 注释 | RMII 9 线，PHY 地址自动扫描 | 串口日志里看 "PHY 地址"，找不到就查 50 MHz REF_CLK |
| 调试串口 | `board.h` | USART1 PA9/PA10 115200 | 启动日志也能用控制台 `log` 命令看 |

另外两点设计上的约定：

- FPGA 同一时刻只认一个主机（最后发命令的那个）；MCU 在跑时用 PC 直连 FPGA 会把事件流抢走。
- 事件时间戳取快滤波过阈的拍，带几拍的幅度走动；做符合测量时要注意。
