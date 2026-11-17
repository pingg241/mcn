"""用 DejaVu Sans Mono 生成 LCD 点阵字库 app/font_data.c（ASCII 32..126）。

    python mcu/tools/gen_font.py [字体文件]
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

OUT = Path(__file__).resolve().parents[1] / "app" / "font_data.c"
CANDIDATES = [r"C:\Windows\Fonts\DejaVuSansMono_0.ttf", r"C:\Windows\Fonts\DejaVuSansMono.ttf",
              "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"]


def render(font_path, w, h, size, thresh=110):
    font = ImageFont.truetype(font_path, size)
    asc, desc = font.getmetrics()
    rows_all = []
    for code in range(32, 127):
        img = Image.new("L", (w, h), 0)
        d = ImageDraw.Draw(img)
        ch = chr(code)
        # 原点取整：小数偏移会让 1 像素宽的横笔画落在两行之间、各剩一半亮度而被阈值吃掉
        x = round((w - font.getlength(ch)) / 2)
        y = (h - (asc + desc)) // 2
        d.text((x, y), ch, fill=255, font=font)
        rows = []
        for yy in range(h):
            bits = 0
            for xx in range(w):
                if img.getpixel((xx, yy)) >= thresh:
                    bits |= 1 << (w - 1 - xx)
            rows.append(bits)
        rows_all.append(rows)
    return rows_all


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    path = sys.argv[1] if len(sys.argv) > 1 else next(p for p in CANDIDATES if Path(p).exists())
    f16 = render(path, 8, 16, 13)    # 14 号字宽 8.4 像素，超出 8 像素格
    f24 = render(path, 12, 24, 20)
    out = ["/* 由 mcu/tools/gen_font.py 生成，字体 DejaVu Sans Mono（Bitstream Vera 许可） */",
           "#include <stdint.h>", "",
           "const uint8_t font8x16[95][16] = {"]
    for c, rows in zip(range(32, 127), f16):
        out.append("    {" + ", ".join(f"0x{r:02X}" for r in rows) + f"}},  /* {chr(c)!r} */")
    out += ["};", "", "const uint16_t font12x24[95][24] = {"]
    for c, rows in zip(range(32, 127), f24):
        out.append("    {" + ", ".join(f"0x{r << 4:04X}" for r in rows) + f"}},  /* {chr(c)!r} */")
    out += ["};", ""]
    OUT.write_text("\n".join(out).replace("/* '*/' */", "/* star-slash */"), encoding="utf-8")
    print("写出", OUT)
    # 预览图：两套字库各一行行排开，放大 3 倍
    sheet = Image.new("L", (16 * 12 * 3, (6 * 16 + 6 * 24 + 8) * 3), 0)
    for font_rows, w, h, y0 in ((f16, 8, 16, 0), (f24, 12, 24, 6 * 16 + 8)):
        for idx, rows in enumerate(font_rows):
            cx, cy = (idx % 16) * 12, y0 + (idx // 16) * h
            for yy, r in enumerate(rows):
                for xx in range(w):
                    if r >> (w - 1 - xx) & 1:
                        for k in range(9):
                            sheet.putpixel(((cx + xx) * 3 + k % 3, (cy + yy) * 3 + k // 3), 255)
    prev = OUT.parent.parent / "build" / "font_preview.png"
    prev.parent.mkdir(exist_ok=True)
    sheet.save(prev)
    print("预览", prev)


if __name__ == "__main__":
    main()
