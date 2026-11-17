/* 帧缓冲绘图（RGB565）。定义 GFX_BYTESWAP=1 时颜色按 SPI 发送顺序（高字节在前）存放。 */
#ifndef GFX_H
#define GFX_H

#include <stdint.h>

#include "platform.h"

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

enum {
    C_BLACK  = RGB565(0, 0, 0),
    C_WHITE  = RGB565(255, 255, 255),
    C_BG     = RGB565(12, 14, 20),
    C_PANEL  = RGB565(28, 32, 44),
    C_GRID   = RGB565(44, 50, 66),
    C_AXIS   = RGB565(140, 150, 170),
    C_TEXT   = RGB565(220, 226, 236),
    C_DIM    = RGB565(130, 138, 156),
    C_SPEC   = RGB565(80, 200, 255),
    C_FILL   = RGB565(24, 70, 110),
    C_PEAK   = RGB565(255, 190, 60),
    C_GOOD   = RGB565(90, 210, 120),
    C_WARN   = RGB565(250, 200, 60),
    C_BAD    = RGB565(240, 80, 80),
    C_WAVE   = RGB565(140, 230, 120),
};

void gfx_init(uint16_t *fb);
uint16_t gfx_color(uint16_t c);    /* 转成帧缓冲存储格式 */
void gfx_fill(int x, int y, int w, int h, uint16_t c);
void gfx_pixel(int x, int y, uint16_t c);
void gfx_hline(int x, int y, int w, uint16_t c);
void gfx_vline(int x, int y, int h, uint16_t c);
void gfx_line(int x0, int y0, int x1, int y1, uint16_t c);
void gfx_rect(int x, int y, int w, int h, uint16_t c);
/* 文字：big=0 8×16，big=1 12×24；bg=GFX_TRANSPARENT 时透明；返回绘制宽度 */
#define GFX_TRANSPARENT 0x0001u
int  gfx_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int big);
int  gfx_printf(int x, int y, uint16_t fg, uint16_t bg, int big, const char *fmt, ...);
int  gfx_text_width(const char *s, int big);

#endif
