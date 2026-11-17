/* 板级定义：STM32H743VIT6 核心板（默认按 WeAct MiniSTM32H7xx）+ ST7796S 4.0" SPI 屏 + LAN8720 RMII 模块。
 * 换板子只改这里。 */
#ifndef BOARD_H
#define BOARD_H

#include "stm32h7xx_hal.h"

/* ---------------- 时钟：25 MHz HSE → SYSCLK 400 MHz（VOS1），AHB 200 MHz，APB 100 MHz ---------------- */
#define BOARD_HSE_HZ 25000000u

/* ---------------- LCD：SPI4，TX only；WeAct 板上 LCD 插座正好是这几脚 ---------------- */
#define LCD_SPI            SPI4
#define LCD_SPI_AF         GPIO_AF5_SPI4
#define LCD_SCK_PORT       GPIOE
#define LCD_SCK_PIN        GPIO_PIN_12
#define LCD_MOSI_PORT      GPIOE
#define LCD_MOSI_PIN       GPIO_PIN_14
#define LCD_CS_PORT        GPIOE
#define LCD_CS_PIN         GPIO_PIN_11
#define LCD_DC_PORT        GPIOE
#define LCD_DC_PIN         GPIO_PIN_13
#define LCD_BL_PORT        GPIOE
#define LCD_BL_PIN         GPIO_PIN_10
#define LCD_HAS_RST        1             /* 屏的 RST 接了 MCU 就置 1；接上电复位电路则置 0，只发软件复位 */
#define LCD_RST_PORT       GPIOE
#define LCD_RST_PIN        GPIO_PIN_9
/* SPI4 内核时钟取 PLL2Q = 80 MHz，二分频 40 MHz（规划里 ST7796S SCLK ≤ 40 MHz）；花屏就改成 _4（20 MHz） */
#define LCD_SPI_PRESCALER  SPI_BAUDRATEPRESCALER_2
#define LCD_MADCTL         0x28          /* 横屏 480×320，BGR；方向反了改 0xE8 */
#define LCD_INVERT         1             /* 选型是 IPS 屏，IPS 版 ST7796 一般要开反显（INVON）；颜色反了改 0 */
#define LCD_DMA_STREAM     DMA1_Stream0
#define LCD_DMA_IRQn       DMA1_Stream0_IRQn

/* ---------------- 以太网：RMII，LAN8720（PHY 地址自动扫描） ---------------- */
/* REF_CLK PA1, MDIO PA2, MDC PC1, CRS_DV PA7, RXD0 PC4, RXD1 PC5, TX_EN PB11, TXD0 PB12, TXD1 PB13 */

/* ---------------- 指示灯与按键 ---------------- */
#define LED_PORT           GPIOE
#define LED_PIN            GPIO_PIN_3
#define LED_ON_LEVEL       GPIO_PIN_SET
#define KEY_PORT           GPIOC
#define KEY_PIN            GPIO_PIN_13
#define KEY_PRESSED_LEVEL  GPIO_PIN_SET  /* WeAct 的 K1 按下接 3.3 V */
#define KEY_PULL           GPIO_PULLDOWN

/* ---------------- 调试串口：USART1 PA9 TX / PA10 RX，115200 ---------------- */
#define LOG_UART           USART1
#define LOG_UART_BAUD      115200u

/* ---------------- 内部闪存：Bank 2 的 8 个 128 KB 扇区作存储槽（槽 0 配置，1..7 能谱） ---------------- */
#define NV_BASE            0x08100000u
#define NV_SECTOR_SIZE     0x20000u
#define NV_SLOTS           8u

#endif
