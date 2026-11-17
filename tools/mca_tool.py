"""MCA 前端的 PC 工具。

上位机（代替 MCU 直接测 FPGA）：
    python mca_tool.py ping
    python mca_tool.py read CTRL          /  write CTRL 3
    python mca_tool.py setup --tau 50 --rise 2 --flat 1 --thr 8 [--test] [--neg]
    python mca_tool.py run --time 60 --out spec.csv [--plot]
    python mca_tool.py wave --src 1 --depth 2 --pre 200 [--force] [--plot]
    python mca_tool.py status
    python mca_tool.py listen             只打印收到的包

假 FPGA（代替 FPGA 测 MCU / 宿主版 MCU 程序）：
    python mca_tool.py fake-fpga --ip 127.0.0.2 [--rate 5000] [--loss 0.01]

MCU 文本控制台（UDP 6000）：
    python mca_tool.py --mcu 192.168.1.20 mcu status          任意控制台命令，help 看命令表
    python mca_tool.py --mcu 192.168.1.20 mcu-spec --out spec.csv

公共参数：--fpga 192.168.1.10（FPGA 地址），--local 0.0.0.0（本机绑定地址）
"""
import argparse
import math
import random
import select
import socket
import struct
import sys
import time

import mcaproto as P

DEFAULT_FPGA = "192.168.1.10"


# =====================================================================
# 上位机
# =====================================================================
class Fpga:
    def __init__(self, ip, local="0.0.0.0"):
        self.ip = ip
        self.cs = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.es = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        for s, port in ((self.cs, P.PORT_CMD), (self.es, P.PORT_EV)):
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
            s.bind((local, port))
        self.cs.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        self.seq = 0
        self.backlog = []          # 等应答期间收到的其他包

    def call(self, cmd, addr=0, value=0, timeout=0.3, retries=3, check=True):
        for _ in range(retries):
            self.seq = (self.seq + 1) & 0xFF
            self.cs.sendto(P.mccm(cmd, self.seq, addr, value), (self.ip, P.PORT_CMD))
            t_end = time.time() + timeout
            while time.time() < t_end:
                r, _, _ = select.select([self.cs, self.es], [], [], max(0.0, t_end - time.time()))
                for s in r:
                    data, _ = s.recvfrom(2048)
                    p = P.parse(data)
                    if p and p["type"] == "MCRS" and p["seq"] == self.seq:
                        if check and p["status"] != P.ST_OK:
                            raise RuntimeError(f"{P.CMD_NAMES.get(cmd, cmd)} 失败：{P.ST_NAMES.get(p['status'])}")
                        return p["status"], p["value"]
                    if p:
                        self.backlog.append(p)
        raise TimeoutError(f"{P.CMD_NAMES.get(cmd, cmd)} 无应答（FPGA {self.ip}）")

    def read(self, name):
        return self.call(P.CMD_READ_REG, P.REG[name] if isinstance(name, str) else name)[1]

    def write(self, name, value):
        return self.call(P.CMD_WRITE_REG, P.REG[name] if isinstance(name, str) else name, value)[1]

    def packets(self, timeout):
        while self.backlog:
            yield self.backlog.pop(0)
        r, _, _ = select.select([self.cs, self.es], [], [], timeout)
        for s in r:
            while True:
                try:
                    s.setblocking(False)
                    data, _ = s.recvfrom(2048)
                except (BlockingIOError, OSError):
                    break
                finally:
                    s.setblocking(True)
                p = P.parse(data)
                if p:
                    yield p


class Acq:
    """事件累积、丢包统计和死时间修正（与 MCU 端 acq.c 的算法一致）。"""

    def __init__(self, nch=4096, shift=4, keep_pileup=False):
        self.nch, self.shift, self.keep_pileup = nch, shift, keep_pileup
        self.hist = [0] * nch
        self.pkts = 0
        self.events = 0
        self.flag_cnt = {P.EV_SAT: 0, P.EV_BASELINE: 0, P.EV_TEST: 0, P.EV_PILEUP: 0}
        self.first_seq = None
        self.max_seq = None
        self.last_st = None
        self.first_st = None

    def on_packet(self, p):
        if p["type"] == "MCEV":
            if self.first_seq is None:
                self.first_seq = p["seq"]
            self.max_seq = p["seq"] if self.max_seq is None else max(self.max_seq, p["seq"])
            self.pkts += 1
            for ts, e, f in p["events"]:
                self.events += 1
                for k in self.flag_cnt:
                    if f & k:
                        self.flag_cnt[k] += 1
                if f & P.EV_PILEUP and not self.keep_pileup:
                    continue
                ch = e >> self.shift
                if ch < self.nch:
                    self.hist[ch] += 1
        elif p["type"] == "MCST":
            if self.first_st is None:
                self.first_st = p
            self.last_st = p

    def lost_packets(self):
        if self.first_seq is None:
            return 0
        return (self.max_seq - self.first_seq + 1) - self.pkts

    def summary(self):
        s = self.last_st
        d = dict(events=self.events, pkts=self.pkts, lost_pkts=self.lost_packets(),
                 counts=sum(self.hist))
        if s:
            real = s["total_ticks"] / P.FS_HZ
            live = (s["total_ticks"] - s["busy_ticks"]) / P.FS_HZ
            d.update(real_s=real, live_s=live,
                     dead_pct=100.0 * s["busy_ticks"] / max(1, s["total_ticks"]),
                     icr=s["total_events"] / real if real else 0.0,
                     fpga_events=s["total_events"], fpga_pileup=s["pileup_events"],
                     fpga_dropped=s["dropped_events"])
            lost_ev = d["lost_pkts"] * (self.events / self.pkts if self.pkts else 0)
            d["loss_factor"] = (self.events + lost_ev + s["dropped_events"]) / self.events if self.events else 1.0
        return d


def cmd_ping(a):
    f = Fpga(a.fpga, a.local)
    t0 = time.time()
    _, v = f.call(P.CMD_PING)
    print(f"PING {a.fpga}: 版本 {v >> 16}.{(v >> 8) & 0xFF}.{v & 0xFF}，往返 {1e3 * (time.time() - t0):.2f} ms")
    print("ID", hex(f.read("ID")))


def parse_int(s):
    return int(s, 0)


def cmd_read(a):
    f = Fpga(a.fpga, a.local)
    name = a.reg.upper()
    v = f.read(name if name in P.REG else parse_int(a.reg))
    print(f"{a.reg} = {v} (0x{v:08x})")


def cmd_write(a):
    f = Fpga(a.fpga, a.local)
    name = a.reg.upper()
    f.write(name if name in P.REG else parse_int(a.reg), parse_int(a.value))
    print("OK")


def cmd_status(a):
    f = Fpga(a.fpga, a.local)
    sf = f.call(P.CMD_GET_STATUS)[1]
    print("status:", P.status_str(sf))
    for n in ("TOTAL_EVENTS_LO", "PILEUP_EVENTS", "DROPPED_EVENTS", "TOTAL_TICKS_LO", "BUSY_TICKS_LO",
              "ADC_SAT_EVENTS", "PKT_SEQ", "RX_FRAMES", "RX_ERRORS"):
        print(f"  {n:16s} {f.read(n)}")
    print(f"  {'BASELINE':16s} {f.read('BASELINE') / 256:.2f} LSB")


def cmd_setup(a):
    f = Fpga(a.fpga, a.local)
    regs = P.filter_params(a.tau, a.rise, a.flat, a.fast, a.gap, a.thr)
    f.write("CTRL", 0)
    for k, v in regs.items():
        f.write(k, v)
        print(f"  {k:13s} = {v}")
    if a.test:
        f.write("TP_RATE", round(a.test_rate / P.FS_HZ * 2**32))
        f.write("TP_AMP", int(a.test_amp * 256))
        decay = max(1, min(15, round(math.log2(a.tau * 32))))   # τ ≈ 2^decay 采样
        f.write("TP_CFG", (3 << 12) | (decay << 8) | (1 << 4) | 1)
    f.call(P.CMD_SET_STREAM, a.n, a.ms)
    f.call(P.CMD_CLEAR_COUNTERS)
    ctrl = P.CTRL_RUN | (P.CTRL_SRC_TEST if a.test else 0) | (P.CTRL_POL_NEG if a.neg else 0)
    f.write("CTRL", ctrl)
    print("已启动，CTRL =", ctrl)


def cmd_run(a):
    f = Fpga(a.fpga, a.local)
    f.call(P.CMD_PING)
    if a.clear:
        f.call(P.CMD_CLEAR_COUNTERS)
    acq = Acq(a.nch, a.shift, a.keep_pileup)
    plt = None
    if a.plot:
        import matplotlib.pyplot as plt
        plt.ion()
        fig, ax = plt.subplots(figsize=(10, 5))
        line, = ax.step(range(a.nch), acq.hist, where="mid", lw=0.8)
        ax.set_yscale("log")
        ax.set_xlabel("channel")
        ax.set_ylabel("counts")
    t0 = time.time()
    t_print = t0
    t_ping = t0
    while time.time() - t0 < a.time:
        for p in f.packets(0.05):
            acq.on_packet(p)
        now = time.time()
        if now - t_ping > 1.0:              # 保持主机身份（FPGA 以最近命令来源为主机）
            try:
                f.call(P.CMD_PING, timeout=0.2, retries=1)
            except TimeoutError:
                pass
            t_ping = now
        if now - t_print >= 1.0:
            s = acq.summary()
            msg = f"[{now - t0:5.1f}s] 事件 {s['events']} 包 {s['pkts']} 丢包 {s['lost_pkts']}"
            if "real_s" in s:
                msg += (f" | 实时 {s['real_s']:.2f}s 活时间 {s['live_s']:.2f}s 死时间 {s['dead_pct']:.1f}%"
                        f" 输入率 {s['icr']:.0f} cps 堆积 {s['fpga_pileup']} FIFO丢 {s['fpga_dropped']}")
            print(msg)
            t_print = now
            if plt:
                line.set_ydata([max(0.5, v) for v in acq.hist])
                ax.set_ylim(0.5, max(10, max(acq.hist) * 2))
                ax.set_title(msg)
                fig.canvas.draw_idle()
                plt.pause(0.001)
    s = acq.summary()
    print("汇总:", {k: (round(v, 4) if isinstance(v, float) else v) for k, v in s.items()})
    if a.out:
        with open(a.out, "w") as fo:
            fo.write(f"# real_s={s.get('real_s', 0):.6f} live_s={s.get('live_s', 0):.6f} "
                     f"loss_factor={s.get('loss_factor', 1):.6f} shift={a.shift}\n")
            fo.write("channel,counts\n")
            for i, v in enumerate(acq.hist):
                fo.write(f"{i},{v}\n")
        print("能谱已保存", a.out)
    if plt:
        plt.ioff()
        plt.show()


def cmd_wave(a):
    f = Fpga(a.fpga, a.local)
    src = a.src | (0x80 if a.force else 0)
    f.call(P.CMD_CAPTURE_WAVE, (src << 24) | (a.depth << 16) | a.pre)
    total = 256 << a.depth
    smp = [None] * total
    t_end = time.time() + a.timeout
    got = 0
    info = None
    while got < total and time.time() < t_end:
        for p in f.packets(0.1):
            if p["type"] == "MCWF":
                info = p
                smp[p["offset"]:p["offset"] + p["n"]] = p["samples"]
                got += p["n"]
    if got < total:
        sys.exit(f"波形不完整 {got}/{total}（没有触发？可加 --force）")
    print(f"波形 src={info['src'] & 3} {total} 点，pretrig {info['pretrig']}，触发时间戳 {info['trig_ts']}")
    if a.out:
        with open(a.out, "w") as fo:
            fo.write("index,value\n")
            for i, v in enumerate(smp):
                fo.write(f"{i},{v}\n")
    if a.plot:
        import matplotlib.pyplot as plt
        t = [(i - info["pretrig"]) * P.TS_NS / 1000 for i in range(total)]
        plt.plot(t, smp, lw=0.8)
        plt.axvline(0, color="r", lw=0.5)
        plt.xlabel("t (µs)")
        plt.title(["原始采样", "极性校正后", "快滤波", "慢滤波（能量单位）"][info["src"] & 3])
        plt.show()


def cmd_listen(a):
    f = Fpga(a.fpga, a.local)
    t0 = time.time()
    while time.time() - t0 < a.time:
        for p in f.packets(0.2):
            if p["type"] == "MCEV":
                print(f"MCEV seq {p['seq']} n {p['n']} 首事件 ts {p['events'][0][0]} E {p['events'][0][1]}")
            elif p["type"] == "MCST":
                print(f"MCST seq {p['seq']} total {p['total_events']} ticks {p['total_ticks']} "
                      f"busy {p['busy_ticks']} {P.status_str(p['status_flags'])}")
            else:
                print(p["type"], {k: v for k, v in p.items() if k not in ("samples", "events")})


# =====================================================================
# 假 FPGA
# =====================================================================
class Spectrum:
    """合成 NaI 能谱：Cs-137 + Co-60 + K-40 + 本底，能量单位 = 30/keV（662 keV ≈ 19860）。"""
    GAIN = 30.0

    def __init__(self, fwhm662=0.07):
        self.r = fwhm662
        self.lines = [(661.7, 1.0, "Cs-137"), (32.0, 0.06, "Ba-K"), (1173.2, 0.35, "Co-60"),
                      (1332.5, 0.35, "Co-60"), (1460.8, 0.05, "K-40")]

    def sigma(self, e):
        return self.r * 661.7 * math.sqrt(e / 661.7) / 2.3548

    def sample(self):
        u = random.random()
        e0, w, _ = random.choices(self.lines, weights=[x[1] for x in self.lines])[0]
        if u < 0.35:                                     # 全能峰
            e = random.gauss(e0, self.sigma(e0))
        elif u < 0.85:                                   # 康普顿平台（近似均匀到康普顿边）
            ec = e0 * (2 * e0 / 511) / (1 + 2 * e0 / 511)
            e = random.uniform(0.05, 1.0) * ec
            e = random.gauss(e, self.sigma(max(e, 20)))
        elif u < 0.92:                                   # 反散射峰
            eb = e0 / (1 + 2 * e0 / 511)
            e = random.gauss(eb, self.sigma(eb) * 1.5)
        else:                                            # 低能本底
            e = random.expovariate(1 / 80.0)
        return max(0, min(65535, int(e * self.GAIN)))


class FakeFpga:
    def __init__(self, ip, rate, loss, fwhm):
        self.cs = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.es = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.cs.bind((ip, P.PORT_CMD))
        self.es.bind((ip, P.PORT_EV))
        self.ip = ip
        self.rate = rate
        self.loss = loss
        self.spec = Spectrum(fwhm)
        self.regs = {v: 0 for v in P.REG.values()}
        defaults = dict(FAST_K=8, FAST_GAP=8, THRESHOLD=64, SLOW_K=64, SLOW_M=32, PZ_M=1023 * 256,
                        PEAK_DELAY=63, PEAK_WIN=42, BLR_SHIFT=9, PILEUP_WIN=112, BLR_HOLD=184,
                        ENERGY_GAIN=32768, ENERGY_SHIFT=17, BL_WIN=0xFF00, TP_RATE=671089,
                        TP_AMP=0x6400, TP_CFG=0x3A11, TP_BASE=16)
        for k, v in defaults.items():
            self.regs[P.REG[k]] = v
        self.host = None
        self.stream_n, self.stream_ms = 64, 1
        self.reset_counters()
        self.t_last = time.time()
        self.ts = 0
        self.last_trig = -10**9
        self.pkt_seq = 0
        self.st_seq = 0
        self.buf = []
        self.buf_t = None
        self.t_hb = time.time()
        self.wave_req = None
        self.wf_id = 0

    def reset_counters(self):
        self.total = self.pileup = self.dropped = self.sat = 0
        self.ticks = self.busy = 0

    @property
    def running(self):
        return self.regs[P.REG["CTRL"]] & P.CTRL_RUN

    def send(self, payload, port, dst=None):
        if dst is None:
            if not self.host:
                return
            dst = (self.host, port)
        if port == P.PORT_EV and self.loss and random.random() < self.loss:
            return                                     # 模拟 UDP 丢包
        (self.es if port == P.PORT_EV else self.cs).sendto(payload, dst)

    def handle(self, data, addr):
        p = P.parse(data)
        if not p or p["type"] != "MCCM":
            return
        if not self.regs.get(-1):                       # 未锁定时学习主机
            self.host = addr[0]
        cmd, seq, a, v = p["cmd"], p["seq"], p["addr"], p["value"]
        st, val = P.ST_OK, 0
        if cmd == P.CMD_PING:
            val = 0x00010000
        elif cmd == P.CMD_READ_REG:
            live = {P.REG["TOTAL_EVENTS_LO"]: self.total, P.REG["PILEUP_EVENTS"]: self.pileup,
                    P.REG["DROPPED_EVENTS"]: self.dropped, P.REG["TOTAL_TICKS_LO"]: self.ticks & 0xFFFFFFFF,
                    P.REG["TOTAL_TICKS_HI"]: self.ticks >> 32, P.REG["BUSY_TICKS_LO"]: self.busy & 0xFFFFFFFF,
                    P.REG["BUSY_TICKS_HI"]: self.busy >> 32, P.REG["ID"]: P.MCA_ID, P.REG["VERSION"]: 0x10000,
                    P.REG["PKT_SEQ"]: self.pkt_seq, P.REG["BASELINE"]: 16 << 8}
            if a in live:
                val = live[a]
            elif a in self.regs:
                val = self.regs[a]
            else:
                st = P.ST_BAD_ADDR
        elif cmd == P.CMD_WRITE_REG:
            if a in self.regs and a < 0x60 and a not in (0, 4, 0xC):
                self.regs[a] = v
                val = v
            else:
                st = P.ST_BAD_ADDR
        elif cmd == P.CMD_CLEAR_COUNTERS:
            self.reset_counters()
        elif cmd == P.CMD_SET_STREAM:
            if 1 <= a <= 64 and 1 <= v <= 10000:
                self.stream_n, self.stream_ms = a, v
            else:
                st = P.ST_BAD_VALUE
        elif cmd == P.CMD_CAPTURE_WAVE:
            self.wave_req = (a >> 24, (a >> 16) & 0xFF, a & 0xFFFF)
        elif cmd == P.CMD_GET_STATUS:
            val = self.status()
        elif cmd == P.CMD_RESET:
            self.regs[P.REG["CTRL"]] = 0
            self.reset_counters()
        elif cmd == P.CMD_SET_NET:
            pass
        else:
            st = P.ST_BAD_CMD
        self.cs.sendto(P.mcrs(cmd, seq, st, val), addr)
        print(f"{P.CMD_NAMES.get(cmd, hex(cmd))} addr=0x{a:x} value={v} → {P.ST_NAMES[st]} {val}")

    def status(self):
        ctrl = self.regs[P.REG["CTRL"]]
        return (ctrl & 1) | (1 << 1) | (2 << 2) | ((self.host is not None) << 4) | ((ctrl >> 1 & 1) << 5)

    def step(self):
        now = time.time()
        dt = now - self.t_last
        self.t_last = now
        n_ticks = int(dt * P.FS_HZ)
        if self.running:
            test = self.regs[P.REG["CTRL"]] & P.CTRL_SRC_TEST
            rate = self.regs[P.REG["TP_RATE"]] / 2**32 * P.FS_HZ if test else self.rate
            w = self.regs[P.REG["PILEUP_WIN"]]
            n = self.poisson(rate * dt)
            times = sorted(random.randrange(self.ts, self.ts + max(1, n_ticks)) for _ in range(n))
            prev = self.last_trig
            for i, t in enumerate(times):
                nxt = times[i + 1] if i + 1 < len(times) else None
                pu = (t - prev < w) or (nxt is not None and nxt - t < w)
                self.busy += min(t - prev, 2 * w)
                prev = t
                if test:
                    e = int(self.regs[P.REG["TP_AMP"]] / 256 * 255.5)
                else:
                    e = self.spec.sample()
                fl = (P.EV_TEST if test else 0) | (P.EV_PILEUP if pu else 0)
                if pu:
                    self.pileup += 1
                    e = min(65535, e + self.spec.sample() // 2)
                self.total += 1
                if not (pu and self.regs[P.REG["CTRL"]] & P.CTRL_PU_DISCARD):
                    self.push((t, e, fl))
            self.last_trig = prev
            self.ticks += n_ticks
        self.ts += n_ticks
        if self.buf and (len(self.buf) >= self.stream_n or now - self.buf_t >= self.stream_ms / 1000):
            self.flush()
        if now - self.t_hb >= 1.0:
            self.t_hb = now
            self.send(P.mcst(self.st_seq, self.pkt_seq, self.total, self.pileup, self.dropped,
                             self.ticks, self.busy, self.sat, self.status()), P.PORT_CMD)
            self.st_seq += 1
        if self.wave_req:
            self.send_wave(*self.wave_req)
            self.wave_req = None

    def push(self, ev):
        if self.buf and (ev[0] >> 32) != (self.buf[0][0] >> 32):
            self.flush()
        if not self.buf:
            self.buf_t = time.time()
        self.buf.append(ev)
        if len(self.buf) >= self.stream_n:
            self.flush()

    def flush(self):
        if not self.host:
            self.dropped += len(self.buf)
        else:
            self.send(P.mcev(self.pkt_seq, self.buf[0][0] >> 32,
                             [(t & 0xFFFFFFFF, e, f) for t, e, f in self.buf]), P.PORT_EV)
        self.pkt_seq += 1
        self.buf = []

    def send_wave(self, src, dc, pre):
        total = 256 << dc
        amp = self.spec.sample() / 256
        smp = []
        for i in range(total):
            t = i - pre
            if (src & 3) in (0, 1):
                v = 16 + (amp * (1 - math.exp(-t / 8)) * math.exp(-t / 1024) if t >= 0 else 0) + random.gauss(0, 0.5)
            else:
                v = amp * 256 * min(1, max(0, t / 64)) if t < 96 else amp * 256 * max(0, 1 - (t - 96) / 64)
            smp.append(int(v))
        self.wf_id += 1
        for off in range(0, total, 256):
            chunk = smp[off:off + 256]
            hdr = struct.pack(">4sBBHIHHIHH", b"MCWF", 1, src, len(chunk), self.wf_id, off, total,
                              self.ts & 0xFFFFFFFF, pre, 0)
            self.send(hdr + struct.pack(f">{len(chunk)}h", *chunk), P.PORT_EV)

    @staticmethod
    def poisson(lam):
        if lam > 50:
            return max(0, int(random.gauss(lam, math.sqrt(lam)) + 0.5))
        k, p, l = 0, 1.0, math.exp(-lam)
        while True:
            p *= random.random()
            if p <= l:
                return k
            k += 1

    def serve(self):
        print(f"假 FPGA 运行于 {self.ip}:{P.PORT_CMD}/{P.PORT_EV}，探测器计数率 {self.rate} cps，丢包率 {self.loss}")
        while True:
            r, _, _ = select.select([self.cs], [], [], 0.002)
            if r:
                data, addr = self.cs.recvfrom(2048)
                self.handle(data, addr)
            self.step()


def cmd_fake(a):
    FakeFpga(a.ip, a.rate, a.loss, a.fwhm).serve()


# ---------------- MCU 控制台 ----------------
def mcu_call(a, line, timeout=2.0):
    """发一行命令，收到以 OK / ERR 开头的行为止，返回全部文本行。"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 22)
    s.bind((a.local, 0))
    s.settimeout(timeout)
    s.sendto((line + "\n").encode(), (a.mcu, P.PORT_CONSOLE))
    text = b""
    try:
        while True:
            data, _ = s.recvfrom(4096)
            text += data                      # 日志等长输出可能在行中间分包，拼起来再分行
            lines = text.decode("utf-8", "replace").splitlines()
            if text.endswith(b"\n") and (lines[-1].startswith("OK") or lines[-1].startswith("ERR")):
                return lines
    except socket.timeout:
        raise SystemExit(f"MCU {a.mcu} 无应答：{line}")
    except ConnectionError:
        # 对方端口没人监听时 Windows 报 10054（ConnectionReset），Linux 报 ConnectionRefused
        raise SystemExit(f"{a.mcu}:{P.PORT_CONSOLE} 上没有程序在收命令：mca_host 没在运行（窗口关了或被 Ctrl+C 了？）")
    finally:
        s.close()


def cmd_mcu(a):
    for line in mcu_call(a, " ".join(a.words), a.timeout):
        print(line)


def cmd_mcu_spec(a):
    lines = mcu_call(a, "spec")
    if not lines[-1].startswith("OK"):
        raise SystemExit(lines[-1])
    _, first, count, nch, total = lines[0].split()
    counts = [int(x) for ln in lines[1:-1] for x in ln.split()]
    assert len(counts) == int(count), (len(counts), count)
    cal = [float(mcu_call(a, f"get cal{i}")[0]) for i in range(3)]
    unit = mcu_call(a, "get cal_unit")[0]
    with open(a.out, "w", encoding="utf-8") as f:
        f.write(f"channel,counts,energy_{unit}\n")
        for ch, c in enumerate(counts):
            f.write(f"{ch},{c},{cal[0] + cal[1] * ch + cal[2] * ch * ch:.4f}\n")
    print(f"{a.out}: {nch} 道，{total} 计数")


# =====================================================================
def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace", line_buffering=True)
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fpga", default=DEFAULT_FPGA)
    ap.add_argument("--local", default="0.0.0.0")
    ap.add_argument("--mcu", default="192.168.1.20", help="MCU 地址（mcu / mcu-spec 用）")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("ping").set_defaults(fn=cmd_ping)
    sub.add_parser("status").set_defaults(fn=cmd_status)
    s = sub.add_parser("read")
    s.add_argument("reg")
    s.set_defaults(fn=cmd_read)
    s = sub.add_parser("write")
    s.add_argument("reg")
    s.add_argument("value")
    s.set_defaults(fn=cmd_write)

    s = sub.add_parser("setup", help="按物理参数配置滤波器并启动")
    s.add_argument("--tau", type=float, default=32.0, help="前放衰减时间常数 µs（测试源用 32）")
    s.add_argument("--rise", type=float, default=2.0)
    s.add_argument("--flat", type=float, default=1.0)
    s.add_argument("--fast", type=float, default=0.25)
    s.add_argument("--gap", type=float, default=0.25)
    s.add_argument("--thr", type=float, default=8.0, help="阈值（LSB）")
    s.add_argument("--n", type=int, default=64, help="每包事件数")
    s.add_argument("--ms", type=int, default=1, help="超时 ms")
    s.add_argument("--test", action="store_true", help="使用内置测试脉冲源")
    s.add_argument("--test-rate", type=float, default=5000)
    s.add_argument("--test-amp", type=float, default=100)
    s.add_argument("--neg", action="store_true", help="负极性输入")
    s.set_defaults(fn=cmd_setup)

    s = sub.add_parser("run", help="采集能谱")
    s.add_argument("--time", type=float, default=10)
    s.add_argument("--out")
    s.add_argument("--plot", action="store_true")
    s.add_argument("--clear", action="store_true")
    s.add_argument("--nch", type=int, default=4096)
    s.add_argument("--shift", type=int, default=4, help="道址 = energy >> shift")
    s.add_argument("--keep-pileup", action="store_true")
    s.set_defaults(fn=cmd_run)

    s = sub.add_parser("wave", help="抓波形")
    s.add_argument("--src", type=int, default=0, help="0 原始 1 极性校正 2 快滤波 3 慢滤波")
    s.add_argument("--depth", type=int, default=2, help="点数 = 256 << depth（0..4）")
    s.add_argument("--pre", type=int, default=128)
    s.add_argument("--force", action="store_true", help="不等脉冲立即触发")
    s.add_argument("--timeout", type=float, default=3)
    s.add_argument("--out")
    s.add_argument("--plot", action="store_true")
    s.set_defaults(fn=cmd_wave)

    s = sub.add_parser("listen")
    s.add_argument("--time", type=float, default=10)
    s.set_defaults(fn=cmd_listen)

    s = sub.add_parser("fake-fpga", help="假 FPGA")
    s.add_argument("--ip", default="127.0.0.2")
    s.add_argument("--rate", type=float, default=5000)
    s.add_argument("--loss", type=float, default=0.0)
    s.add_argument("--fwhm", type=float, default=0.07, help="662 keV 处相对 FWHM")
    s.set_defaults(fn=cmd_fake)

    s = sub.add_parser("mcu", help="MCU 控制台命令")
    s.add_argument("words", nargs="+")
    s.add_argument("--timeout", type=float, default=2.0)
    s.set_defaults(fn=cmd_mcu)
    s = sub.add_parser("mcu-spec", help="从 MCU 下载能谱为 CSV")
    s.add_argument("--out", default="spec.csv")
    s.set_defaults(fn=cmd_mcu_spec)

    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
