/* platform.h 在 STM32H743 上的实现（网络部分在 net.c） */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "console.h"
#include "lcd_st7796.h"
#include "platform.h"

extern UART_HandleTypeDef huart_log;

/* 帧缓冲在 AXI SRAM，32 字节对齐便于按 cache 行写回 */
static uint16_t fb[LCD_W * LCD_H] __attribute__((aligned(32)));

uint32_t plat_now_ms(void) { return HAL_GetTick(); }

uint16_t *plat_framebuffer(void) { return fb; }
void plat_lcd_flush(int x, int y, int w, int h)
{
    (void)x; (void)w;                      /* 只按整行推送，帧缓冲行是连续内存 */
    lcd_flush_rows(fb, y, h);
}
int plat_lcd_busy(void) { return lcd_busy(); }

uint32_t plat_keys(void) { return HAL_GPIO_ReadPin(KEY_PORT, KEY_PIN) == KEY_PRESSED_LEVEL ? 1u : 0u; }
void plat_led(int on)
{
    GPIO_PinState off = LED_ON_LEVEL == GPIO_PIN_SET ? GPIO_PIN_RESET : GPIO_PIN_SET;
    HAL_GPIO_WritePin(LED_PORT, LED_PIN, on ? LED_ON_LEVEL : off);
}

void plat_log(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    console_log(buf);
    HAL_UART_Transmit(&huart_log, (uint8_t *)buf, (uint16_t)n, 50);
}

/* ---------------- 内部闪存存储槽（Bank 2，每槽一个 128 KB 扇区） ---------------- */
unsigned plat_nv_slots(void) { return NV_SLOTS; }
size_t plat_nv_slot_size(void) { return NV_SECTOR_SIZE; }

int plat_nv_erase(unsigned slot)
{
    if (slot >= NV_SLOTS) return -1;
    FLASH_EraseInitTypeDef e = {0};
    uint32_t err = 0;
    e.TypeErase = FLASH_TYPEERASE_SECTORS;
    e.Banks = FLASH_BANK_2;
    e.Sector = slot;
    e.NbSectors = 1;
    e.VoltageRange = FLASH_VOLTAGE_RANGE_3;
    HAL_FLASH_Unlock();
    HAL_StatusTypeDef st = HAL_FLASHEx_Erase(&e, &err);   /* 约 1~2 s，期间主循环停住 */
    HAL_FLASH_Lock();
    SCB_InvalidateDCache_by_Addr((uint32_t *)(NV_BASE + slot * NV_SECTOR_SIZE), (int32_t)NV_SECTOR_SIZE);
    return st == HAL_OK ? 0 : -1;
}

int plat_nv_write(unsigned slot, size_t off, const void *data, size_t len)
{
    if (slot >= NV_SLOTS || off % 32 || off + len > NV_SECTOR_SIZE) return -1;
    uint32_t addr = NV_BASE + slot * NV_SECTOR_SIZE + (uint32_t)off;
    const uint8_t *p = (const uint8_t *)data;
    uint32_t word[8] __attribute__((aligned(32)));
    HAL_StatusTypeDef st = HAL_OK;
    HAL_FLASH_Unlock();
    for (size_t i = 0; i < len && st == HAL_OK; i += 32) {
        size_t n = len - i < 32 ? len - i : 32;
        memset(word, 0xFF, sizeof(word));
        memcpy(word, p + i, n);
        st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD, addr + (uint32_t)i, (uint32_t)word);
    }
    HAL_FLASH_Lock();
    SCB_InvalidateDCache_by_Addr((uint32_t *)(addr & ~31u), (int32_t)((len + 63u) & ~31u));
    return st == HAL_OK ? 0 : -1;
}

int plat_nv_read(unsigned slot, size_t off, void *data, size_t len)
{
    if (slot >= NV_SLOTS || off + len > NV_SECTOR_SIZE) return -1;
    memcpy(data, (const void *)(NV_BASE + slot * NV_SECTOR_SIZE + off), len);
    return 0;
}
