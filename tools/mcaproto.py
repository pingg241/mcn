"""MCA 协议定义（与 docs/protocol.md、fpga/rtl/mca_regs.vh、mcu/app/mca_proto.h 保持一致）。

大端字节序；包以 4 字节 ASCII magic 开头；时间戳 32 MHz 采样计数（31.25 ns/tick）。
"""
import math
import struct
import zlib

FS_HZ = 32_000_000
TS_NS = 1e9 / FS_HZ

PORT_CMD = 5000          # MCCM/MCRS/MCST
PORT_EV = 5001           # MCEV/MCWF
PORT_CONSOLE = 6000      # MCU 文本控制台

# ---------------- 命令 ----------------
CMD_PING, CMD_WRITE_REG, CMD_READ_REG, CMD_CLEAR_COUNTERS = 0x01, 0x02, 0x03, 0x04
CMD_SET_STREAM, CMD_CAPTURE_WAVE, CMD_GET_STATUS, CMD_RESET, CMD_SET_NET = 0x05, 0x06, 0x07, 0x08, 0x09
CMD_NAMES = {1: "PING", 2: "WRITE_REG", 3: "READ_REG", 4: "CLEAR_COUNTERS", 5: "SET_STREAM",
             6: "CAPTURE_WAVE", 7: "GET_STATUS", 8: "RESET", 9: "SET_NET"}
ST_OK, ST_BAD_CMD, ST_BAD_ADDR, ST_BAD_VALUE, ST_BUSY = 0, 1, 2, 3, 4
ST_NAMES = {0: "OK", 1: "BAD_CMD", 2: "BAD_ADDR", 3: "BAD_VALUE", 4: "BUSY"}

# SET_NET 参数索引
NET_LOCAL_IP, NET_LOCAL_MAC_HI, NET_LOCAL_MAC_LO = 0, 1, 2
NET_HOST_IP, NET_HOST_MAC_HI, NET_HOST_MAC_LO, NET_HOST_VALID, NET_HOST_LOCK = 3, 4, 5, 6, 7

# ---------------- 寄存器 ----------------
REG = dict(
    ID=0x00, VERSION=0x04, CTRL=0x08, STATUS=0x0C,
    FAST_K=0x10, FAST_GAP=0x14, THRESHOLD=0x18,
    SLOW_K=0x20, SLOW_M=0x24, PZ_M=0x28, PEAK_DELAY=0x2C, PEAK_WIN=0x30,
    BLR_SHIFT=0x34, PILEUP_WIN=0x38, BLR_HOLD=0x3C,
    ENERGY_GAIN=0x40, ENERGY_SHIFT=0x44, BL_WIN=0x48, TOT_MAX=0x4C,
    TP_RATE=0x50, TP_AMP=0x54, TP_CFG=0x58, TP_BASE=0x5C,
    TOTAL_EVENTS_LO=0x100, TOTAL_EVENTS_HI=0x104, PILEUP_EVENTS=0x108, DROPPED_EVENTS=0x10C,
    TOTAL_TICKS_LO=0x110, TOTAL_TICKS_HI=0x114, BUSY_TICKS_LO=0x118, BUSY_TICKS_HI=0x11C,
    ADC_SAT_EVENTS=0x120, BASELINE=0x124, TIMESTAMP_LO=0x128, TIMESTAMP_HI=0x12C,
    PKT_SEQ=0x130, NET_STATUS=0x134, STREAM_N=0x138, STREAM_MS=0x13C,
    LOCAL_IP=0x140, HOST_IP=0x144, RX_FRAMES=0x148, RX_ERRORS=0x14C,
)
CTRL_RUN, CTRL_SRC_TEST, CTRL_POL_NEG, CTRL_PU_DISCARD, CTRL_PEAK_FIXED = 1, 2, 4, 8, 16
MCA_ID = 0x4D434131

# 事件 flags
EV_SAT, EV_BASELINE, EV_TEST, EV_PILEUP = 1, 2, 4, 8
# status_flags
SF = dict(RUN=1 << 0, LINK=1 << 1, HOST_VALID=1 << 4, SRC_TEST=1 << 5, WAVE_BUSY=1 << 6,
          FIFO_OVF=1 << 7, ADC_SAT=1 << 8, BASELINE_BAD=1 << 9, PU_DISCARD=1 << 10,
          INHIBIT=1 << 11, HOST_LOCK=1 << 12)
SPEED_NAMES = {0: "none", 1: "10M", 2: "100M", 3: "1000M"}


def status_str(sf):
    names = [k for k, v in SF.items() if sf & v]
    return f"0x{sf:08x} speed={SPEED_NAMES[(sf >> 2) & 3]} " + "|".join(names)


# ---------------- 打包 / 解析 ----------------
def mccm(cmd, seq, addr=0, value=0):
    return struct.pack(">4sBBHII", b"MCCM", cmd, seq & 0xFF, 0, addr & 0xFFFFFFFF, value & 0xFFFFFFFF)


def mcrs(cmd, seq, status, value=0):
    return struct.pack(">4sBBBBII", b"MCRS", cmd, seq & 0xFF, status, 0, value & 0xFFFFFFFF, 0)


def mcev(seq, ts_epoch, events):
    """events: [(ts_low, energy, flags)]"""
    hdr = struct.pack(">4sBBHIHH", b"MCEV", 1, 0, len(events), seq & 0xFFFFFFFF, ts_epoch & 0xFFFF, 0)
    return hdr + b"".join(struct.pack(">IHBB", t & 0xFFFFFFFF, e, f, 0) for t, e, f in events)


def mcst(seq, pkt_seq, total, pileup, dropped, ticks, busy, sat, status):
    return struct.pack(">4sBBHIIQIIQQII8x", b"MCST", 1, 0, 0, seq, pkt_seq, total, pileup, dropped,
                       ticks, busy, sat, status)


def parse(payload):
    """解析任一包，返回 dict（含 'type'），无法识别返回 None。"""
    if len(payload) < 4:
        return None
    m = payload[:4]
    if m == b"MCEV":
        if len(payload) < 16:
            return None
        _, ver, fl, n, seq, epoch, _ = struct.unpack(">4sBBHIHH", payload[:16])
        if len(payload) < 16 + 8 * n:
            return None
        evs = []
        for i in range(n):
            t, e, f, _ = struct.unpack(">IHBB", payload[16 + 8 * i:24 + 8 * i])
            evs.append(((epoch << 32) | t, e, f))
        return dict(type="MCEV", version=ver, n=n, seq=seq, epoch=epoch, events=evs)
    if m == b"MCST" and len(payload) >= 64:
        f = struct.unpack(">4sBBHIIQIIQQII8x", payload[:64])
        return dict(type="MCST", version=f[1], seq=f[4], pkt_seq=f[5], total_events=f[6],
                    pileup_events=f[7], dropped_events=f[8], total_ticks=f[9], busy_ticks=f[10],
                    adc_sat_events=f[11], status_flags=f[12])
    if m == b"MCRS" and len(payload) >= 16:
        _, cmd, seq, st, _, val, _ = struct.unpack(">4sBBBBII", payload[:16])
        return dict(type="MCRS", cmd=cmd, seq=seq, status=st, value=val)
    if m == b"MCCM" and len(payload) >= 16:
        _, cmd, seq, _, addr, val = struct.unpack(">4sBBHII", payload[:16])
        return dict(type="MCCM", cmd=cmd, seq=seq, addr=addr, value=val)
    if m == b"MCWF" and len(payload) >= 24:
        _, ver, src, n, cid, off, total, ts, pre, _ = struct.unpack(">4sBBHIHHIHH", payload[:24])
        smp = list(struct.unpack(f">{n}h", payload[24:24 + 2 * n]))
        return dict(type="MCWF", version=ver, src=src, n=n, capture_id=cid, offset=off,
                    total=total, trig_ts=ts, pretrig=pre, samples=smp)
    return None


# ---------------- 滤波参数换算 ----------------
def filter_params(tau_us, rise_us=2.0, flat_us=1.0, fast_us=0.25, fast_gap_us=0.25,
                  thr_lsb=8.0, full_scale=65280):
    """由物理参数算 FPGA 寄存器值。

    tau_us   前放衰减时间常数（µs）
    rise_us  梯形上升时间，flat_us 平顶
    thr_lsb  触发阈值（脉冲幅度 LSB）
    full_scale  ADC 满幅 255 LSB 对应的能量值
    """
    ts = 1.0 / FS_HZ
    k = max(1, min(1023, round(rise_us * 1e-6 / ts)))
    m = max(0, min(255, round(flat_us * 1e-6 / ts)))
    kf = max(1, min(63, round(fast_us * 1e-6 / ts)))
    gap = max(0, min(63, round(fast_gap_us * 1e-6 / ts)))
    M = 1.0 / math.expm1(ts / (tau_us * 1e-6))
    mq = min(0xFFFFFF, round(M * 256))
    m_eff = mq / 256.0
    # H = A·k·(M+1)·256；E = ((H >>> S)·G) >> 14，要求 A=255 → full_scale
    ratio = full_scale / 255.0 / (k * (m_eff + 1) * 256)
    shift, gain = 0, 0
    for s in range(48):
        g = ratio * 2 ** (s + 14)
        if g >= 65535.5:
            break
        shift, gain = s, round(g)
    return dict(
        FAST_K=kf, FAST_GAP=gap, THRESHOLD=max(1, round(thr_lsb * kf)),
        SLOW_K=k, SLOW_M=m, PZ_M=mq,
        PEAK_DELAY=max(0, k + 7 - kf), PEAK_WIN=m + kf + 2,
        PILEUP_WIN=min(4095, k + m + kf + 8), BLR_HOLD=min(4095, 2 * k + m + kf + 16),
        ENERGY_GAIN=gain, ENERGY_SHIFT=shift,
    )


# ---------------- 以太网帧（仿真/调试用） ----------------
def mac_bytes(mac):
    if isinstance(mac, (bytes, bytearray)):
        return bytes(mac)
    return bytes(int(x, 16) for x in mac.split(":"))


def ip_bytes(ip):
    if isinstance(ip, (bytes, bytearray)):
        return bytes(ip)
    return bytes(int(x) for x in ip.split("."))


def csum16(b):
    if len(b) % 2:
        b = b + b"\0"
    s = sum(struct.unpack(f">{len(b) // 2}H", b))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return s


def fcs(frame):
    return struct.pack("<I", zlib.crc32(frame) & 0xFFFFFFFF)


def eth_frame(dst, src, etype, payload, pad=True):
    f = mac_bytes(dst) + mac_bytes(src) + struct.pack(">H", etype) + payload
    if pad and len(f) < 60:
        f += b"\0" * (60 - len(f))
    return f + fcs(f)


def udp_frame(dst_mac, src_mac, src_ip, dst_ip, sport, dport, payload, ident=0):
    sip, dip = ip_bytes(src_ip), ip_bytes(dst_ip)
    ulen = 8 + len(payload)
    pseudo = sip + dip + struct.pack(">BBH", 0, 17, ulen)
    uh = struct.pack(">HHHH", sport, dport, ulen, 0)
    c = (~csum16(pseudo + uh + payload)) & 0xFFFF or 0xFFFF
    uh = struct.pack(">HHHH", sport, dport, ulen, c)
    ih = struct.pack(">BBHHHBBH4s4s", 0x45, 0, 20 + ulen, ident, 0x4000, 64, 17, 0, sip, dip)
    ih = ih[:10] + struct.pack(">H", (~csum16(ih)) & 0xFFFF) + ih[12:]
    return eth_frame(dst_mac, src_mac, 0x0800, ih + uh + payload)


def arp_request(src_mac, src_ip, target_ip):
    p = struct.pack(">HHBBH", 1, 0x0800, 6, 4, 1) + mac_bytes(src_mac) + ip_bytes(src_ip) \
        + b"\0" * 6 + ip_bytes(target_ip)
    return eth_frame("ff:ff:ff:ff:ff:ff", src_mac, 0x0806, p)


def parse_frame(f):
    """解析带 FCS 的以太网帧，返回 dict；校验失败字段放在 'errors' 里。"""
    r = dict(errors=[])
    if len(f) < 64:
        r["errors"].append(f"帧长 {len(f)} < 64")
    if len(f) < 18:
        return r
    body, fc = f[:-4], f[-4:]
    if fcs(body) != fc:
        r["errors"].append("FCS 错")
    r["dst"], r["src"] = body[0:6], body[6:12]
    et = struct.unpack(">H", body[12:14])[0]
    r["etype"] = et
    if et == 0x0806:
        htype, ptype, hl, pl, op = struct.unpack(">HHBBH", body[14:22])
        r.update(kind="ARP", op=op, sha=body[22:28], spa=body[28:32], tha=body[32:38], tpa=body[38:42])
        if (htype, ptype, hl, pl) != (1, 0x0800, 6, 4):
            r["errors"].append("ARP 头错")
    elif et == 0x0800:
        ih = body[14:34]
        if csum16(ih) != 0xFFFF:
            r["errors"].append("IP 校验和错")
        tot = struct.unpack(">H", ih[2:4])[0]
        sip, dip = ih[12:16], ih[16:20]
        u = body[34:14 + tot]
        sport, dport, ulen, uc = struct.unpack(">HHHH", u[:8])
        if ulen != len(u):
            r["errors"].append(f"UDP 长度 {ulen} != {len(u)}")
        pseudo = sip + dip + struct.pack(">BBH", 0, 17, ulen)
        if uc != 0 and csum16(pseudo + u) != 0xFFFF:
            r["errors"].append("UDP 校验和错")
        r.update(kind="UDP", sip=sip, dip=dip, sport=sport, dport=dport, payload=u[8:])
    else:
        r["kind"] = "OTHER"
    return r
