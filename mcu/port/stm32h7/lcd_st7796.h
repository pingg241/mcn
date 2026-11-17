/* ST7796S 480×320 SPI 屏：命令阻塞发送，像素数据 DMA 分块发送（帧缓冲已是高字节在前，GFX_BYTESWAP=1） */
#ifndef LCD_ST7796_H
#define LCD_ST7796_H

#include <stdint.h>

void lcd_init(void);
/* 推送帧缓冲第 y0..y0+h-1 行（整行），DMA 后台进行 */
void lcd_flush_rows(const uint16_t *fb, int y0, int h);
int  lcd_busy(void);
void lcd_backlight(int on);

#endif
