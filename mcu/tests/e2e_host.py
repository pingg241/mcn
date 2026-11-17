"""宿主版 MCU 程序 + 假 FPGA 端到端测试。

    python mcu/tests/e2e_host.py [mca_host 可执行文件]

假 FPGA 在 127.0.0.2（1% 丢包、20 kcps 合成 NaI 谱），宿主版在 127.0.0.1；
经 UDP 6000 控制台切到探测器输入、寻峰、两点刻度、下载能谱、存取、抓波形，
最后宿主版把三页界面存成 BMP（mcu/build/e2e/shot_*.bmp）。
"""
import subprocess
import sys
import time
import types
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import mca_tool  # noqa: E402

OUT = ROOT / "mcu" / "build" / "e2e"
GAIN = 30.0 / 16            # 假 FPGA 30 能量单位/keV，4096 道（>>4）→ 1.875 道/keV
RUN_S = 40

fails = 0


def check(cond, msg):
    global fails
    print(("  ok   " if cond else "  FAIL ") + msg)
    fails += 0 if cond else 1


def call(line, timeout=3.0):
    a = types.SimpleNamespace(mcu="127.0.0.1", local="127.0.0.1")
    return mca_tool.mcu_call(a, line, timeout)


def kv(lines):
    return {ln.split()[0]: ln.split()[1] for ln in lines if len(ln.split()) == 2}


def peaks():
    out = []
    for ln in call("peaks")[1:-1]:
        f = ln.split()
        out.append(dict(c=float(f[1]), fwhm=float(f[3]), area=float(f[4]), err=float(f[5]), e=float(f[7]),
                        res=float(f[9])))
    return out


def nearest(pk, ch):
    return min(pk, key=lambda p: abs(p["c"] - ch)) if pk else None


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace", line_buffering=True)
    default = ("mingw", "mca_host.exe") if sys.platform == "win32" else ("linux", "mca_host")
    exe = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "mcu" / "build" / default[0] / "x86_64" / "release" / default[1]
    OUT.mkdir(parents=True, exist_ok=True)
    for f in (OUT / "nv").glob("*"):
        f.unlink()
    fake_log = open(OUT / "fake.log", "w", encoding="utf-8")
    host_log = open(OUT / "host.log", "w", encoding="utf-8")
    fake = subprocess.Popen([sys.executable, str(ROOT / "tools" / "mca_tool.py"), "fake-fpga", "--ip", "127.0.0.2",
                             "--rate", "20000", "--loss", "0.01"], stdout=fake_log, stderr=subprocess.STDOUT)
    time.sleep(0.5)
    host = subprocess.Popen([str(exe), "--fpga", "127.0.0.2", "--local", "127.0.0.1", "--seconds", str(RUN_S),
                             "--bmp", str(OUT / "shot"), "--nv", str(OUT / "nv"), "--key-at", "30000,35000", "--no-window"],
                            stdout=host_log, stderr=subprocess.STDOUT)
    t0 = time.time()
    try:
        time.sleep(2)
        st = kv(call("status"))
        check(st.get("state") == "RUN" and st.get("running") == "1", f"发现并配置 FPGA：state={st.get('state')}")
        check(call("set test_src 0")[-1] == "OK", "切到探测器输入（set test_src 0）")

        time.sleep(18)
        st = kv(call("status"))
        counts, lf = int(st["counts"]), float(st["loss_factor"])
        print(f"  counts={counts} icr={st['icr']} rate={st['rate']} dead={st['dead_pct']}% "
              f"lost_pkt={st['lost_pkt_pct']}% loss_factor={lf}")
        check(counts > 150000, f"进谱计数 {counts}")
        check(0.4 < float(st["lost_pkt_pct"]) < 2.0, f"丢包率 {st['lost_pkt_pct']}%（假 FPGA 设 1%）")
        check(abs(lf - 1 / 0.99) < 0.006, f"丢失补偿系数 {lf:.4f}（期望约 1.0101）")
        rate = float(st["rate"])
        check(abs(rate - 20000) / 20000 < 0.05, f"死时间校正计数率 {rate:.0f} cps（假 FPGA 20000）")

        pk = peaks()
        for p in pk:
            print(f"    峰 ch {p['c']:8.2f} ≈ {p['c'] / GAIN:7.1f} keV  FWHM {p['fwhm']:.2f} ch "
                  f"({100 * p['fwhm'] / p['c']:.2f}%)  N {p['area']:.0f}±{p['err']:.0f}")
        cs, co2 = nearest(pk, 661.7 * GAIN), nearest(pk, 1332.5 * GAIN)
        check(cs and abs(cs["c"] - 661.7 * GAIN) < 2, f"Cs-137 峰位 {cs and cs['c']:.2f}（期望 {661.7 * GAIN:.2f}）")
        check(cs and abs(cs["fwhm"] / cs["c"] - 0.07) < 0.01, f"Cs-137 分辨率 {cs and 100 * cs['fwhm'] / cs['c']:.2f}%（7%）")
        r = call(f"cal {cs['c']:.3f} 661.657 {co2['c']:.3f} 1332.492")
        check(r[-1] == "OK", "两点刻度：" + r[0])
        time.sleep(0.3)
        co1 = nearest(peaks(), 1173.2 * GAIN)
        check(co1 and abs(co1["e"] - 1173.2) < 3, f"刻度后 Co-60 1173 峰能量 {co1 and co1['e']:.2f} keV")

        a = types.SimpleNamespace(mcu="127.0.0.1", local="127.0.0.1", out=str(OUT / "spec.csv"))
        mca_tool.cmd_mcu_spec(a)
        rows = (OUT / "spec.csv").read_text(encoding="utf-8").splitlines()
        check(len(rows) == 4097 and rows[0] == "channel,counts,energy_keV", f"能谱 CSV {len(rows) - 1} 道")

        before = int(kv(call("status"))["counts"])
        check(call("store 1 e2e")[-1] == "OK", "存谱到槽 1")
        check(call("recall 1")[-1] == "OK", "调出槽 1")
        st = kv(call("status"))
        check(st["frozen"] == "1" and before <= int(st["counts"]) < before + 20000 and st["run_wanted"] == "0",
              f"调出后冻结显示：counts={st['counts']} frozen={st['frozen']}")
        slots = call("slots")
        check(slots[0].startswith("1 e2e nch=4096"), "slots：" + slots[0])
        check(call("start")[-1] == "OK", "重新开始（清谱）")
        time.sleep(1.5)
        st = kv(call("status"))
        check(st["frozen"] == "0" and st["running"] == "1", f"恢复采集 counts={st['counts']}")

        w = call("wave 1 2 256", timeout=6)
        hdr = w[0].split()
        n = sum(len(ln.split()) for ln in w[1:-1])
        check(hdr[0] == "WAVE" and hdr[2] == "1024" and n == 1024, f"抓波形：{w[0]}，{n} 点")
        r = call("reg 0x0")
        check(r[0].startswith("0x4D434131"), "读 ID 寄存器：" + r[0])
        check(call("bogus")[-1].startswith("ERR"), "未知命令报错")
    finally:
        left = RUN_S + 3 - (time.time() - t0)
        try:
            host.wait(timeout=max(1, left))
        except subprocess.TimeoutExpired:
            host.kill()
        fake.kill()
        fake_log.close()
        host_log.close()
    for s in ("spectrum", "wave", "stats"):
        check((OUT / f"shot_{s}.bmp").exists(), f"截图 shot_{s}.bmp")
    print("全部通过" if not fails else f"{fails} 项失败")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
