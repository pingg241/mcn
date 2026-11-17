"""xsim 仿真入口。

用法（在任意目录）：
    python fpga/sim/run_sim.py tb_pulse [--plusargs rate=2684354 amp=25600]
    python fpga/sim/run_sim.py tb_top
仿真产物在 fpga/sim/work/<tb>/ 下。tb_top 仿真的是顶层 Block Design：第一次运行或 block_design.tcl、
rtl/ 有改动时，先用 Vivado 生成仿真版工程 fpga/build/mca_sim/（约半分钟）。
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
FPGA = HERE.parent
RTL = FPGA / "rtl"
BD_TCL = FPGA / "scripts" / "block_design.tcl"
BD_EXPORT = FPGA / "build" / "mca_sim" / "export" / "mca_block" / "xsim"
VIVADO_BIN = Path(os.environ.get("VIVADO_BIN", r"E:\Xilinx\Vivado\2023.2\bin"))

BENCHES = {
    "tb_pulse": ["common.v", "test_pulser.v", "pulse_proc.v"],
    "tb_top": None,                     # rtl/ 全部 + Block Design 生成的文件
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


def bd_files(work):
    """顶层 Block Design 的仿真文件（verilog 列表、vhdl 列表、xelab 要的库）。

    block_design.tcl 的 sim 模式建 build/mca_sim 工程并导出 xsim 文件列表；只含 Block Design 生成的
    文件，rtl/ 的源码由调用方自己编译。两个复位 IP 用 Vivado 预编译好的库，靠导出的 xsim.ini 找到。
    """
    stamp = BD_EXPORT / "vlog.prj"
    newest = max(f.stat().st_mtime for f in [BD_TCL, *RTL.glob("*.v*")])
    if not stamp.exists() or stamp.stat().st_mtime < newest:
        print("生成仿真用的 Block Design 工程 build/mca_sim …")
        run([tool("vivado"), "-mode", "batch", "-nojournal", "-nolog", "-source", str(BD_TCL),
             "-tclargs", "sim"], FPGA)
    shutil.copy(BD_EXPORT / "xsim.ini", work)

    def listed(prj):
        return [str((BD_EXPORT / f).resolve()) for f in re.findall(r'"([^"]+)"', (BD_EXPORT / prj).read_text())
                if not f.endswith("glbl.v")]
    elab = (BD_EXPORT / "mca_block.sh").read_text()
    libs = [l for l in dict.fromkeys(re.findall(r"-L (\S+)", elab)) if l != "xil_defaultlib"]
    return listed("vlog.prj"), listed("vhdl.prj"), libs


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

    vhdl, libs = [], []
    if BENCHES[a.tb] is None:
        bd_vlog, vhdl, libs = bd_files(work)
        srcs = sorted(str(f) for f in RTL.glob("*.v")) + bd_vlog
    else:
        srcs = [str(RTL / f) for f in BENCHES[a.tb]]
    srcs += [str(HERE / f) for f in TB_EXTRA.get(a.tb, [])]
    srcs.append(str(HERE / f"{a.tb}.v"))
    uses_unisim = a.tb != "tb_pulse"
    if uses_unisim:
        glbl = VIVADO_BIN.parent / "data" / "verilog" / "src" / "glbl.v"
        srcs.append(str(glbl))

    run([tool("xvlog"), "--nolog", "-i", str(RTL)] + srcs, work)
    if vhdl:
        run([tool("xvhdl"), "--nolog"] + vhdl, work)
    tops = [a.tb] + (["glbl"] if uses_unisim else [])
    if uses_unisim and "unisims_ver" not in libs:
        libs.append("unisims_ver")
    libs = [x for l in libs for x in ("-L", l)]
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
