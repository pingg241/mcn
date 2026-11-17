"""xsim 仿真入口。

用法（在任意目录）：
    python fpga/sim/run_sim.py tb_pulse [--plusargs rate=2684354 amp=25600]
    python fpga/sim/run_sim.py tb_top
仿真产物在 fpga/sim/work/<tb>/ 下。
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
RTL = HERE.parent / "rtl"
VIVADO_BIN = Path(os.environ.get("VIVADO_BIN", r"E:\Xilinx\Vivado\2023.2\bin"))

RTL_ALL = [
    "common.v", "clk_gen.v", "adc_if.v", "test_pulser.v", "pulse_proc.v",
    "wave_capture.v", "adc_domain.v", "rgmii_if.v", "eth_rx_parser.v",
    "eth_tx_engine.v", "mcev_packer.v", "eth_core.v", "mca_top.v",
]

BENCHES = {
    "tb_pulse": ["common.v", "test_pulser.v", "pulse_proc.v"],
    "tb_top": RTL_ALL,
}
TB_EXTRA = {
    "tb_top": ["phy_model.v", "ad9280_model.v"],
}


def tool(name):
    exe = VIVADO_BIN / (name + (".bat" if os.name == "nt" else ""))
    if not exe.exists():
        sys.exit(f"找不到 {exe}，用环境变量 VIVADO_BIN 指定 Vivado bin 目录")
    return str(exe)


def run(cmd, cwd):
    print(">", Path(cmd[0]).name, " ".join(cmd[1:])[:200])
    if os.name == "nt":
        # .bat 会把 '=' 当分隔符，这类参数要带引号原样传入
        cmd = " ".join(f'"{c}"' if (" " in c or "=" in c) else c for c in cmd)
    p = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    lines = []
    for raw in p.stdout.splitlines():
        try:
            lines.append(raw.decode("utf-8"))
        except UnicodeDecodeError:
            lines.append(raw.decode("gbk", errors="replace"))   # Vivado 自身消息按系统代码页
    out = "\n".join(lines)
    lines = [l for l in lines if not l.startswith("ECHO")]
    keep = [l for l in lines if ("ERROR" in l or "WARNING" in l or "CRITICAL" in l
                                 or not l.startswith(("INFO", "Compiling", "Analyzing", "Starting",
                                                      "Completed", "Determining", "Built", "Waiting",
                                                      "Time Resolution", "Multi-threading", "source",
                                                      "# ", "run -all", "exit", "Vivado Simulator",
                                                      "  ****", "    **", "Copyright", "Running:")))]
    print("\n".join(keep))
    if p.returncode != 0:
        sys.exit(f"失败 (返回码 {p.returncode})")
    return out


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace", line_buffering=True)
    ap = argparse.ArgumentParser()
    ap.add_argument("tb", choices=sorted(BENCHES))
    ap.add_argument("--plusargs", nargs="*", default=[])
    ap.add_argument("--clean", action="store_true")
    ap.add_argument("--giga", type=int, default=1, help="tb_top：1 千兆，0 百兆")
    a = ap.parse_args()

    work = HERE / "work" / (a.tb + ("" if a.tb != "tb_top" else ("_1g" if a.giga else "_100m")))
    if a.clean and work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True, exist_ok=True)

    srcs = [str(RTL / f) for f in BENCHES[a.tb]]
    srcs += [str(HERE / f) for f in TB_EXTRA.get(a.tb, [])]
    srcs.append(str(HERE / f"{a.tb}.v"))
    uses_unisim = a.tb != "tb_pulse"
    if uses_unisim:
        glbl = VIVADO_BIN.parent / "data" / "verilog" / "src" / "glbl.v"
        srcs.append(str(glbl))

    run([tool("xvlog"), "--nolog", "-i", str(RTL)] + srcs, work)
    tops = [a.tb] + (["glbl"] if uses_unisim else [])
    libs = ["-L", "unisims_ver"] if uses_unisim else []
    gen = ["-generic_top", f"GIGA={a.giga}"] if a.tb == "tb_top" else []
    run([tool("xelab"), "--nolog", "-debug", "off", "--timescale", "1ns/1ps", "-s", a.tb + "_sim"]
        + gen + libs + tops, work)
    frames = HERE / "tb_top_frames.py"
    if a.tb == "tb_top":
        subprocess.run([sys.executable, str(frames), "gen", str(work)], check=True)
    cmd = [tool("xsim"), "--nolog", a.tb + "_sim", "-R"]
    for pa in a.plusargs:
        cmd += ["-testplusarg", pa]
    run(cmd, work)
    if a.tb == "tb_top":
        r = subprocess.run([sys.executable, str(frames), "check", str(work), "--giga", str(a.giga)])
        sys.exit(r.returncode)


if __name__ == "__main__":
    main()
