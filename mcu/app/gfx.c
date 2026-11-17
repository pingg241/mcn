#include "gfx.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifndef GFX_BYTESWAP
#define GFX_BYTESWAP 0
#endif

extern const uint8_t  font8x16[95][16];
extern const uint16_t font12x24[95][24];

static uint16_t *fb;

void gfx_init(uint16_t *buf) { fb = buf; }

uint16_t gfx_color(uint16_t c)
{
#if GFX_BYTESWAP
    return (uint16_t)((c << 8) | (c >> 8));
#else
    return c;
#endif
}

void gfx_fill(int x, int y, int w, int h, uint16_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_W) w = LCD_W - x;
    if (y + h > LCD_H) h = LCD_H - y;
    if (w <= 0 || h <= 0) return;
    c = gfx_color(c);
    for (int j = 0; j < h; j++) {
        uint16_t *p = fb + (y + j) * LCD_W + x;
        for (int i = 0; i < w; i++) p[i] = c;
    }
}

void gfx_pixel(int x, int y, uint16_t c)
{
    if ((unsigned)x < LCD_W && (unsigned)y < LCD_H) fb[y * LCD_W + x] = gfx_color(c);
}

void gfx_hline(int x, int y, int w, uint16_t c) { gfx_fill(x, y, w, 1, c); }
void gfx_vline(int x, int y, int h, uint16_t c) { gfx_fill(x, y, 1, h, c); }

void gfx_line(int x0, int y0, int x1, int y1, uint16_t c)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        gfx_pixel(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void gfx_rect(int x, int y, int w, int h, uint16_t c)
{
    gfx_hline(x, y, w, c);
    gfx_hline(x, y + h - 1, w, c);
    gfx_vline(x, y, h, c);
    gfx_vline(x + w - 1, y, h, c);
}

int gfx_text_width(const char *s, int big) { return (int)strlen(s) * (big ? 12 : 8); }

int gfx_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int big)
{
    const int cw = big ? 12 : 8, chh = big ? 24 : 16;
    uint16_t f = gfx_color(fg), b = gfx_color(bg);
    int x0 = x;
    for (; *s; s++, x += cw) {
        unsigned code = (unsigned char)*s;
        if (code < 32 || code > 126) code = '?';
        for (int r = 0; r < chh; r++) {
            int yy = y + r;
            if ((unsigned)yy >= LCD_H) continue;
            uint32_t bits = big ? font12x24[code - 32][r] : (uint32_t)font8x16[code - 32][r] << 8;
            for (int k = 0; k < cw; k++) {
                int xx = x + k;
                if ((unsigned)xx >= LCD_W) continue;
                if (bits & (0x8000u >> k)) fb[yy * LCD_W + xx] = f;
                else if (bg != GFX_TRANSPARENT) fb[yy * LCD_W + xx] = b;
            }
        }
    }
    return x - x0;
}

int gfx_printf(int x, int y, uint16_t fg, uint16_t bg, int big, const char *fmt, ...)
{
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return gfx_text(x, y, buf, fg, bg, big);
}
