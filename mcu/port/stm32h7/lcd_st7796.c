#include "lcd_st7796.h"

#include "board.h"
#include "platform.h"

SPI_HandleTypeDef hspi_lcd;
DMA_HandleTypeDef hdma_lcd;

#define CHUNK_ROWS 32          /* 32 行 = 30720 字节，H7 SPI 一次最多 65535 */

static volatile int busy;
static const uint8_t *dma_ptr;
static uint32_t dma_left;

static void cs(int level) { HAL_GPIO_WritePin(LCD_CS_PORT, LCD_CS_PIN, level ? GPIO_PIN_SET : GPIO_PIN_RESET); }
static void dc(int level) { HAL_GPIO_WritePin(LCD_DC_PORT, LCD_DC_PIN, level ? GPIO_PIN_SET : GPIO_PIN_RESET); }

static void cmd(uint8_t c, const uint8_t *data, uint16_t n)
{
    cs(0);
    dc(0);
    HAL_SPI_Transmit(&hspi_lcd, &c, 1, 10);
    if (n) {
        dc(1);
        HAL_SPI_Transmit(&hspi_lcd, (uint8_t *)data, n, 10);
    }
    cs(1);
}

#define CMD(c, ...)                                     \
    do {                                                \
        static const uint8_t d_[] = {__VA_ARGS__};      \
        cmd(c, d_, sizeof(d_));                         \
    } while (0)

static void hw_init(void)
{
    GPIO_InitTypeDef g = {0};
    __HAL_RCC_SPI4_CLK_ENABLE();
    __HAL_RCC_DMA1_CLK_ENABLE();

    g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = LCD_SPI_AF;
    g.Pin = LCD_SCK_PIN;
    HAL_GPIO_Init(LCD_SCK_PORT, &g);
    g.Pin = LCD_MOSI_PIN;
    HAL_GPIO_Init(LCD_MOSI_PORT, &g);

    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    g.Alternate = 0;
    HAL_GPIO_WritePin(LCD_CS_PORT, LCD_CS_PIN, GPIO_PIN_SET);
    g.Pin = LCD_CS_PIN;
    HAL_GPIO_Init(LCD_CS_PORT, &g);
    g.Pin = LCD_DC_PIN;
    HAL_GPIO_Init(LCD_DC_PORT, &g);
    HAL_GPIO_WritePin(LCD_BL_PORT, LCD_BL_PIN, GPIO_PIN_RESET);
    g.Pin = LCD_BL_PIN;
    HAL_GPIO_Init(LCD_BL_PORT, &g);
#if LCD_HAS_RST
    HAL_GPIO_WritePin(LCD_RST_PORT, LCD_RST_PIN, GPIO_PIN_SET);
    g.Pin = LCD_RST_PIN;
    HAL_GPIO_Init(LCD_RST_PORT, &g);
#endif

    hspi_lcd.Instance = LCD_SPI;
    hspi_lcd.Init.Mode = SPI_MODE_MASTER;
    hspi_lcd.Init.Direction = SPI_DIRECTION_2LINES_TXONLY;
    hspi_lcd.Init.DataSize = SPI_DATASIZE_8BIT;
    hspi_lcd.Init.CLKPolarity = SPI_POLARITY_LOW;
    hspi_lcd.Init.CLKPhase = SPI_PHASE_1EDGE;
    hspi_lcd.Init.NSS = SPI_NSS_SOFT;
    hspi_lcd.Init.BaudRatePrescaler = LCD_SPI_PRESCALER;
    hspi_lcd.Init.FirstBit = SPI_FIRSTBIT_MSB;
    hspi_lcd.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi_lcd.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    hspi_lcd.Init.NSSPMode = SPI_NSS_PULSE_DISABLE;
    hspi_lcd.Init.FifoThreshold = SPI_FIFO_THRESHOLD_01DATA;
    hspi_lcd.Init.MasterKeepIOState = SPI_MASTER_KEEP_IO_STATE_ENABLE;   /* 空闲时 SCK 保持低 */
    if (HAL_SPI_Init(&hspi_lcd) != HAL_OK) return;

    hdma_lcd.Instance = LCD_DMA_STREAM;
    hdma_lcd.Init.Request = DMA_REQUEST_SPI4_TX;
    hdma_lcd.Init.Direction = DMA_MEMORY_TO_PERIPH;
    hdma_lcd.Init.PeriphInc = DMA_PINC_DISABLE;
    hdma_lcd.Init.MemInc = DMA_MINC_ENABLE;
    hdma_lcd.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma_lcd.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    hdma_lcd.Init.Mode = DMA_NORMAL;
    hdma_lcd.Init.Priority = DMA_PRIORITY_LOW;
    hdma_lcd.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&hdma_lcd) != HAL_OK) return;
    __HAL_LINKDMA(&hspi_lcd, hdmatx, hdma_lcd);

    HAL_NVIC_SetPriority(LCD_DMA_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(LCD_DMA_IRQn);
    HAL_NVIC_SetPriority(SPI4_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(SPI4_IRQn);
}

void lcd_backlight(int on) { HAL_GPIO_WritePin(LCD_BL_PORT, LCD_BL_PIN, on ? GPIO_PIN_SET : GPIO_PIN_RESET); }

void lcd_init(void)
{
    hw_init();
#if LCD_HAS_RST
    HAL_GPIO_WritePin(LCD_RST_PORT, LCD_RST_PIN, GPIO_PIN_RESET);
    HAL_Delay(10);
    HAL_GPIO_WritePin(LCD_RST_PORT, LCD_RST_PIN, GPIO_PIN_SET);
    HAL_Delay(120);
#endif
    /* 初始化序列参照 TFT_eSPI 的 ST7796_Init.h */
    cmd(0x01, 0, 0);                       /* 软件复位 */
    HAL_Delay(120);
    cmd(0x11, 0, 0);                       /* 退出睡眠 */
    HAL_Delay(120);
    CMD(0xF0, 0xC3);                       /* 打开扩展命令 */
    CMD(0xF0, 0x96);
    CMD(0x36, LCD_MADCTL);
    CMD(0x3A, 0x55);                       /* RGB565 */
    CMD(0xB4, 0x01);
    CMD(0xB6, 0x80, 0x02, 0x3B);
    CMD(0xE8, 0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33);
    CMD(0xC1, 0x06);
    CMD(0xC2, 0xA7);
    CMD(0xC5, 0x18);
    HAL_Delay(120);
    CMD(0xE0, 0xF0, 0x09, 0x0B, 0x06, 0x04, 0x15, 0x2F, 0x54, 0x42, 0x3C, 0x17, 0x14, 0x18, 0x1B);
    CMD(0xE1, 0xE0, 0x09, 0x0B, 0x06, 0x04, 0x03, 0x2B, 0x43, 0x42, 0x3B, 0x16, 0x14, 0x17, 0x1B);
    HAL_Delay(120);
    CMD(0xF0, 0x3C);                       /* 关闭扩展命令 */
    CMD(0xF0, 0x69);
    HAL_Delay(120);
    cmd(LCD_INVERT ? 0x21 : 0x20, 0, 0);
    cmd(0x29, 0, 0);                       /* 开显示 */
    lcd_backlight(1);
}

static void start_chunk(void)
{
    uint32_t n = dma_left > CHUNK_ROWS * LCD_W * 2u ? CHUNK_ROWS * LCD_W * 2u : dma_left;
    const uint8_t *p = dma_ptr;
    dma_ptr += n;
    dma_left -= n;
    if (HAL_SPI_Transmit_DMA(&hspi_lcd, (uint8_t *)p, (uint16_t)n) != HAL_OK) {
        cs(1);
        busy = 0;
    }
}

void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *h)
{
    if (h != &hspi_lcd) return;
    if (dma_left) start_chunk();
    else {
        cs(1);
        busy = 0;
    }
}

void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *h)
{
    if (h != &hspi_lcd) return;
    cs(1);
    busy = 0;
}

void lcd_flush_rows(const uint16_t *fb, int y0, int h)
{
    if (busy || h <= 0) return;
    if (y0 < 0) { h += y0; y0 = 0; }
    if (y0 + h > LCD_H) h = LCD_H - y0;
    const uint16_t y1 = (uint16_t)(y0 + h - 1);
    const uint8_t ca[4] = {0, 0, (LCD_W - 1) >> 8, (LCD_W - 1) & 0xFF};
    const uint8_t ra[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1};
    cmd(0x2A, ca, 4);
    cmd(0x2B, ra, 4);

    dma_ptr = (const uint8_t *)(fb + y0 * LCD_W);
    dma_left = (uint32_t)h * LCD_W * 2u;
    /* 帧缓冲在可缓存的 AXI SRAM：先把 D-Cache 写回，DMA 才读得到最新内容 */
    uintptr_t a = (uintptr_t)dma_ptr & ~31u;
    SCB_CleanDCache_by_Addr((uint32_t *)a, (int32_t)(((uintptr_t)dma_ptr + dma_left - a + 31u) & ~31u));

    busy = 1;
    cs(0);
    dc(0);
    uint8_t c = 0x2C;                     /* 写显存 */
    HAL_SPI_Transmit(&hspi_lcd, &c, 1, 10);
    dc(1);
    start_chunk();
}

int lcd_busy(void) { return busy; }
