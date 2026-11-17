"""拉取 STM32 固件依赖到 mcu/third_party/（浅克隆固定版本）。

    python mcu/scripts/fetch_deps.py
"""
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / "third_party"
DEPS = [
    # (目录, 仓库, 标签, 稀疏检出子目录)
    ("stm32h7xx_hal_driver", "https://github.com/STMicroelectronics/stm32h7xx_hal_driver.git", "v1.11.7", None),
    ("cmsis_device_h7", "https://github.com/STMicroelectronics/cmsis_device_h7.git", "v1.10.8", None),
    ("cmsis_core", "https://github.com/STMicroelectronics/cmsis_core.git", "v5.9.0_20250520", ["Include"]),
    ("lwip", "https://github.com/STMicroelectronics/stm32_mw_lwip.git", "v2.2.1_20250804", None),
]


def git(*args, cwd=None):
    print("git", " ".join(args))
    subprocess.run(["git", *args], cwd=cwd, check=True)


def main():
    ROOT.mkdir(parents=True, exist_ok=True)
    for name, url, tag, sparse in DEPS:
        dst = ROOT / name
        if (dst / ".git").exists():
            print(f"{name}: 已存在，跳过（删除目录可重新拉取）")
            continue
        if dst.exists():
            shutil.rmtree(dst)
        if sparse:
            git("clone", "--depth", "1", "--branch", tag, "--filter=blob:none", "--sparse", url, str(dst))
            git("sparse-checkout", "set", *sparse, cwd=dst)
        else:
            git("clone", "--depth", "1", "--branch", tag, url, str(dst))
    print("完成")


if __name__ == "__main__":
    sys.exit(main())
