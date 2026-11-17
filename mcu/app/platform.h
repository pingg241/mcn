/* 平台接口：app/ 下的代码只通过这些函数访问硬件，STM32 与 PC 宿主各自实现。 */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#define LCD_W 480
#define LCD_H 320

/* 大块工作区放到 STM32 的 D2 SRAM（AXI SRAM 留给帧缓冲和其余数据），宿主版无效果 */
#if defined(__arm__)
#define PLAT_BIG_BSS __attribute__((section(".sram1_bss")))
#else
#define PLAT_BIG_BSS
#endif

uint32_t plat_now_ms(void);

/* 从本机 local_port 发 UDP 到 dst_ip:dst_port；IP 按 a.b.c.d = 0xAABBCCDD */
int plat_udp_send(uint16_t local_port, uint32_t dst_ip, uint16_t dst_port, const void *data, size_t len);
/* 本机网络：链路速率（0 断开，10/100），本机 IP */
int      plat_link_speed(void);
uint32_t plat_local_ip(void);

/* 帧缓冲 LCD_W × LCD_H RGB565，flush 把矩形区域推到屏幕；busy 时不要改帧缓冲 */
uint16_t *plat_framebuffer(void);
void plat_lcd_flush(int x, int y, int w, int h);
int  plat_lcd_busy(void);

/* 按键位图：bit0 = KEY（切换页面） */
uint32_t plat_keys(void);
void     plat_led(int on);

/* 非易失存储：槽 0 存配置，其余存能谱；返回 0 成功。
 * 写之前先整槽擦除；写入偏移须为 32 的倍数（STM32H7 闪存按 32 字节编程），长度不足时补 0xFF。 */
unsigned plat_nv_slots(void);
size_t   plat_nv_slot_size(void);
int      plat_nv_erase(unsigned slot);
int      plat_nv_write(unsigned slot, size_t off, const void *data, size_t len);
int      plat_nv_read(unsigned slot, size_t off, void *data, size_t len);

void plat_log(const char *fmt, ...);

#endif
