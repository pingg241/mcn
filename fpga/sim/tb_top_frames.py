"""tb_top 的激励生成与结果检查。

    python tb_top_frames.py gen   <work_dir>     生成 rx_frames.txt
    python tb_top_frames.py check <work_dir> [--giga 0|1]
"""
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import mcaproto as P  # noqa: E402

FPGA_MAC, FPGA_IP = "02:00:4d:43:41:01", "192.168.1.10"
MCU_MAC, MCU_IP = "02:00:00:00:00:20", "192.168.1.20"
MCU_PORT = 40000
END_NS = 5_200_000

TP_AMP = 40.0                       # 测试源幅度（LSB）
ADC_AMPS = [20.0, 40.0, 60.0, 80.0, 100.0]
GAIN = 255.5                        # 能量/LSB（256 × 弹道亏损约 0.998）


def cmd(seq, code, addr=0, value=0, bcast=False):
    pl = P.mccm(code, seq, addr, value)
    if bcast:
        return P.udp_frame("ff:ff:ff:ff:ff:ff", MCU_MAC, MCU_IP, "255.255.255.255",
                           MCU_PORT, P.PORT_CMD, pl)
    return P.udp_frame(FPGA_MAC, MCU_MAC, MCU_IP, FPGA_IP, MCU_PORT, P.PORT_CMD, pl, ident=seq)


R = P.REG
SCRIPT = [
    # (时刻 ns, seq, 帧, 期望状态, 期望值或 None)
    (250_000, None, P.arp_request(MCU_MAC, MCU_IP, FPGA_IP), None, None),
    (270_000, 1, cmd(1, P.CMD_PING), P.ST_OK, 0x00010000),
    (290_000, 2, cmd(2, P.CMD_READ_REG, R["ID"]), P.ST_OK, P.MCA_ID),
    (310_000, 3, cmd(3, P.CMD_WRITE_REG, R["FAST_K"], 0), P.ST_BAD_VALUE, None),
    (330_000, 4, cmd(4, P.CMD_READ_REG, 0x1C), P.ST_BAD_ADDR, None),
    (350_000, 5, cmd(5, 0x77), P.ST_BAD_CMD, None),
    (370_000, 6, cmd(6, P.CMD_SET_STREAM, 16, 1), P.ST_OK, None),
    (390_000, 7, cmd(7, P.CMD_WRITE_REG, R["TP_RATE"], round(40e3 / P.FS_HZ * 2**32)), P.ST_OK, None),
    (410_000, 8, cmd(8, P.CMD_WRITE_REG, R["TP_AMP"], int(TP_AMP * 256)), P.ST_OK, None),
    (430_000, 9, cmd(9, P.CMD_WRITE_REG, R["TP_CFG"], 0x3A10), P.ST_OK, 0x3A10),
    (450_000, 20, cmd(20, P.CMD_PING, bcast=True), P.ST_OK, 0x00010000),
    (470_000, 10, cmd(10, P.CMD_WRITE_REG, R["CTRL"], P.CTRL_RUN | P.CTRL_SRC_TEST), P.ST_OK, 3),
    (1_500_000, 11, cmd(11, P.CMD_CAPTURE_WAVE, (3 << 24) | (0 << 16) | 64), P.ST_OK, None),
    (2_000_000, 12, cmd(12, P.CMD_WRITE_REG, R["CTRL"], P.CTRL_RUN), P.ST_OK, 1),
    (3_900_000, 13, cmd(13, P.CMD_GET_STATUS), P.ST_OK, None),
    (3_920_000, 15, cmd(15, P.CMD_READ_REG, R["BUSY_TICKS_LO"]), P.ST_OK, None),
    (3_940_000, 16, cmd(16, P.CMD_SET_NET, P.NET_HOST_LOCK, 1), P.ST_OK, 1),
    (3_960_000, 17, cmd(17, P.CMD_READ_REG, R["NET_STATUS"]), P.ST_OK, None),
    (4_000_000, 18, cmd(18, P.CMD_WRITE_REG, R["CTRL"], 0), P.ST_OK, 0),       # 停止，尾包靠超时发出
    (5_100_000, 14, cmd(14, P.CMD_READ_REG, R["TOTAL_EVENTS_LO"]), P.ST_OK, None),
]


def gen(work):
    with open(Path(work) / "rx_frames.txt", "w") as f:
        for t, _, fr, _, _ in SCRIPT:
            f.write(f"{t} {len(fr)} " + " ".join(f"{b:02x}" for b in fr) + "\n")
        f.write(f"{END_NS} 0\n")


class Checker:
    def __init__(self):
        self.fail = 0

    def ok(self, cond, msg):
        if not cond:
            self.fail += 1
            print("  ✗", msg)
        return cond


def check(work, giga):
    c = Checker()
    lines = (Path(work) / "tx_frames.txt").read_text().splitlines()
    print(f"收到 {len(lines)} 帧")
    arps, rs, ev_pk, st, wf = [], {}, [], [], []
    for ln in lines:
        parts = ln.split()
        t = int(parts[0])
        fr = bytes(int(x, 16) for x in parts[1:])
        r = P.parse_frame(fr)
        c.ok(not r["errors"], f"@{t} 帧错误 {r['errors']}")
        c.ok(r["src"] == P.mac_bytes(FPGA_MAC), f"@{t} 源 MAC 错")
        if r.get("kind") == "ARP":
            arps.append(r)
            continue
        if r.get("kind") != "UDP":
            c.ok(False, f"@{t} 未知帧类型 0x{r['etype']:04x}")
            continue
        c.ok(r["sip"] == P.ip_bytes(FPGA_IP), f"@{t} 源 IP 错")
        c.ok(r["dst"] == P.mac_bytes(MCU_MAC) and r["dip"] == P.ip_bytes(MCU_IP), f"@{t} 目的地址错")
        p = P.parse(r["payload"])
        if p is None:
            c.ok(False, f"@{t} 载荷无法解析")
            continue
        p["t"] = t
        if p["type"] == "MCRS":
            c.ok(r["sport"] == P.PORT_CMD and r["dport"] == MCU_PORT, "MCRS 端口错")
            c.ok(p["seq"] not in rs, f"MCRS seq {p['seq']} 重复")
            rs[p["seq"]] = p
        elif p["type"] == "MCEV":
            c.ok(r["sport"] == P.PORT_EV and r["dport"] == P.PORT_EV, "MCEV 端口错")
            ev_pk.append(p)
        elif p["type"] == "MCST":
            c.ok(r["sport"] == P.PORT_CMD and r["dport"] == P.PORT_CMD, "MCST 端口错")
            st.append(p)
        elif p["type"] == "MCWF":
            wf.append(p)

    print("== ARP")
    if c.ok(len(arps) == 1, f"ARP 应答数 {len(arps)} != 1"):
        a = arps[0]
        c.ok(a["op"] == 2 and a["sha"] == P.mac_bytes(FPGA_MAC) and a["spa"] == P.ip_bytes(FPGA_IP)
             and a["tha"] == P.mac_bytes(MCU_MAC) and a["tpa"] == P.ip_bytes(MCU_IP), "ARP 应答字段错")
        print("  ARP 应答正确")

    print("== 命令应答")
    for t, seq, _, est, eval_ in SCRIPT:
        if seq is None:
            continue
        p = rs.get(seq)
        if not c.ok(p is not None, f"seq {seq} 无应答"):
            continue
        good = p["status"] == est and (eval_ is None or p["value"] == eval_)
        c.ok(good, f"seq {seq}: status={P.ST_NAMES.get(p['status'])} value=0x{p['value']:08x}，"
                   f"期望 {P.ST_NAMES[est]} {'' if eval_ is None else hex(eval_)}")
        lat = (p["t"] - t) / 1000
        print(f"  seq {seq:2d} {P.CMD_NAMES.get(p['cmd'], hex(p['cmd'])):14s} "
              f"{P.ST_NAMES.get(p['status']):9s} 0x{p['value']:08x}  延迟 {lat:.1f} µs")
    if 13 in rs:
        sf = rs[13]["value"]
        print("  status:", P.status_str(sf))
        c.ok(sf & P.SF["RUN"] and sf & P.SF["LINK"] and sf & P.SF["HOST_VALID"], "status 位缺失")
    if 14 in rs:
        n_rx = sum(p["n"] for p in ev_pk)
        print(f"  FPGA 计数 total_events={rs[14]['value']}，收到事件 {n_rx}")
        c.ok(rs[14]["value"] == n_rx, "收到的事件数与 FPGA 计数不一致（有丢失或未冲出）")
        c.ok(((sf >> 2) & 3) == (3 if giga else 2), "status 速率错")
    if 17 in rs:
        c.ok(rs[17]["value"] & (1 << 5), "host_lock 未生效")

    print("== 事件包")
    seqs = [p["seq"] for p in ev_pk]
    c.ok(seqs == list(range(len(seqs))), f"MCEV 序号不连续 {seqs[:10]}...")
    evs = [e for p in ev_pk for e in p["events"]]
    c.ok(all(p["n"] <= 16 for p in ev_pk), "SET_STREAM 16 未生效")
    ts = [e[0] for e in evs]
    c.ok(all(b > a for a, b in zip(ts, ts[1:])), "时间戳不单调")
    for p in ev_pk:
        c.ok(all((e[0] >> 32) == p["epoch"] for e in p["events"]), "epoch 不一致")
    print(f"  {len(ev_pk)} 包 {len(evs)} 事件，每包事件数 {[p['n'] for p in ev_pk]}")
    c.ok(ev_pk and ev_pk[-1]["n"] < 16, "最后一包应由超时发出（不满 16）")

    tp = [e for e in evs if e[2] & P.EV_TEST]
    tpc = [e[1] for e in tp if not e[2] & (P.EV_PILEUP | P.EV_SAT)]
    if c.ok(len(tpc) > 20, f"测试源无堆积事件太少 {len(tpc)}"):
        med = statistics.median(tpc)
        exp = TP_AMP * GAIN
        print(f"  测试源：{len(tp)} 事件，无堆积 {len(tpc)}，能量中位数 {med:.0f}（期望 {exp:.0f}），"
              f"偏差 {100 * (med / exp - 1):+.2f}%，四分位 {statistics.quantiles(tpc, n=4)}")
        c.ok(abs(med / exp - 1) < 0.005, "测试源能量偏差 > 0.5%")

    ad = [e for e in evs if not e[2] & P.EV_TEST]
    adc = [e for e in ad if not e[2] & (P.EV_PILEUP | P.EV_SAT)]
    print(f"  ADC：{len(ad)} 事件，无堆积 {len(adc)}")
    if c.ok(len(adc) >= 40, "ADC 事件太少"):
        # 按注入顺序匹配幅度
        inj = [ln.split() for ln in (Path(work) / "adc_inj.txt").read_text().splitlines()]
        amp_of = {}
        errs = []
        for e in adc:
            a = min(ADC_AMPS, key=lambda x: abs(e[1] - x * GAIN))
            errs.append(e[1] / (a * GAIN) - 1)
            amp_of.setdefault(a, []).append(e[1])
        for a in ADC_AMPS:
            v = amp_of.get(a, [])
            if v:
                print(f"    A={a:5.1f} LSB: n={len(v):2d} E 均值 {statistics.mean(v):8.1f}"
                      f"（期望 {a * GAIN:.0f}）")
        c.ok(max(abs(x) for x in errs) < 0.01, f"ADC 能量误差最大 {max(abs(x) for x in errs):.3%}")
        dts = [b[0] - a[0] for a, b in zip(adc, adc[1:])]
        # 前沿定时：不同幅度过阈时刻相差几个采样（幅度游走）
        walk = max(abs(d - 1280 * round(d / 1280)) for d in dts)
        print(f"    事件间隔相对 1280 的最大偏差 {walk} 采样（前沿定时幅度游走）")
        c.ok(walk <= 8, f"ADC 事件间隔异常 {dts[:8]}")
        seq_amp = [min(ADC_AMPS, key=lambda x: abs(e[1] - x * GAIN)) for e in adc]
        nxt = all(ADC_AMPS[(ADC_AMPS.index(a) + round((b_[0] - a_[0]) / 1280)) % 5] == b
                  for (a, b, a_, b_) in zip(seq_amp, seq_amp[1:], adc, adc[1:]))
        c.ok(nxt, "ADC 幅度顺序与注入不符")
        print(f"    注入 {len(inj)} 个，最大相对误差 {max(abs(x) for x in errs):.3%}")

    print("== 心跳")
    c.ok(len(st) >= 3, f"MCST 只有 {len(st)} 个")
    c.ok([p["seq"] for p in st] == list(range(len(st))), "MCST seq 不连续")
    for p in st:
        n_before = sum(1 for q in ev_pk if q["t"] < p["t"])
        c.ok(p["pkt_seq"] >= n_before, "pkt_seq 小于已收包数")
        c.ok(p["busy_ticks"] <= p["total_ticks"], "busy > total")
        print(f"  MCST seq {p['seq']} @{p['t'] / 1e3:.0f} µs: pkt_seq {p['pkt_seq']} total {p['total_events']} "
              f"pileup {p['pileup_events']} dropped {p['dropped_events']} ticks {p['total_ticks']} "
              f"busy {p['busy_ticks']} sat {p['adc_sat_events']}  {P.status_str(p['status_flags'])}")
    if st and 14 in rs:
        c.ok(rs[14]["value"] >= st[-1]["total_events"], "READ TOTAL_EVENTS 小于最后心跳")

    print("== 波形")
    if c.ok(len(wf) == 1, f"MCWF 包数 {len(wf)}"):
        w = wf[0]
        c.ok(w["n"] == 256 and w["total"] == 256 and w["pretrig"] == 64 and w["src"] == 3, "MCWF 头错")
        pk = max(w["samples"])
        print(f"  {w['n']} 点，src {w['src']}，峰值 {pk}，峰位 {w['samples'].index(pk)}")
        c.ok(pk > 0.9 * TP_AMP * GAIN, "波形峰值不对")

    print("通过" if c.fail == 0 else f"失败 {c.fail} 项")
    return c.fail == 0


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace", line_buffering=True)
    if sys.argv[1] == "gen":
        gen(sys.argv[2])
    else:
        giga = int(sys.argv[sys.argv.index("--giga") + 1]) if "--giga" in sys.argv else 1
        sys.exit(0 if check(sys.argv[2], giga) else 1)
