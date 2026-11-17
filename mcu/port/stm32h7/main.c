/* STM32H743 固件入口：时钟、MPU/缓存、外设初始化，主循环轮询网络与 app */
#include "app.h"
#include "board.h"
#include "lcd_st7796.h"
#include "net.h"
#include "platform.h"

UART_HandleTypeDef huart_log;

void Error_Handler(void)
{
    __disable_irq();
    for (;;) {
        HAL_GPIO_TogglePin(LED_PORT, LED_PIN);
        for (volatile uint32_t i = 0; i < 4000000; i++) {}
    }
}

/* 25 MHz HSE → PLL1 800 MHz VCO：P 400 MHz（SYSCLK），Q 200 MHz；PLL2Q 80 MHz 给 SPI4 */
static void clock_config(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};
    RCC_PeriphCLKInitTypeDef pclk = {0};

    HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
    while (!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

    osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState = RCC_HSE_ON;
    osc.PLL.PLLState = RCC_PLL_ON;
    osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLM = BOARD_HSE_HZ / 5000000u;     /* 5 MHz 鉴相 */
    osc.PLL.PLLN = 160;
    osc.PLL.PLLP = 2;
    osc.PLL.PLLQ = 4;
    osc.PLL.PLLR = 2;
    osc.PLL.PLLRGE = RCC_PLL1VCIRANGE_2;
    osc.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
    osc.PLL.PLLFRACN = 0;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) Error_Handler();

    clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2 |
                    RCC_CLOCKTYPE_D3PCLK1 | RCC_CLOCKTYPE_D1PCLK1;
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clk.SYSCLKDivider = RCC_SYSCLK_DIV1;
    clk.AHBCLKDivider = RCC_HCLK_DIV2;
    clk.APB3CLKDivider = RCC_APB3_DIV2;
    clk.APB1CLKDivider = RCC_APB1_DIV2;
    clk.APB2CLKDivider = RCC_APB2_DIV2;
    clk.APB4CLKDivider = RCC_APB4_DIV2;
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2) != HAL_OK) Error_Handler();

    pclk.PeriphClockSelection = RCC_PERIPHCLK_SPI45;
    pclk.PLL2.PLL2M = BOARD_HSE_HZ / 5000000u;
    pclk.PLL2.PLL2N = 64;                          /* 320 MHz VCO */
    pclk.PLL2.PLL2P = 4;
    pclk.PLL2.PLL2Q = 4;                           /* 80 MHz */
    pclk.PLL2.PLL2R = 4;
    pclk.PLL2.PLL2RGE = RCC_PLL2VCIRANGE_2;
    pclk.PLL2.PLL2VCOSEL = RCC_PLL2VCOWIDE;
    pclk.PLL2.PLL2FRACN = 0;
    pclk.Spi45ClockSelection = RCC_SPI45CLKSOURCE_PLL2;
    if (HAL_RCCEx_PeriphCLKConfig(&pclk) != HAL_OK) Error_Handler();
}

/* 0 号区：整个 4 GB 禁止访问（防 Cortex-M7 投机读到未映射地址）；
 * SRAM2 128 KB 普通内存不可缓存（以太网缓冲、LwIP 堆），其开头 512 B 为 Device（DMA 描述符） */
static void mpu_config(void)
{
    MPU_Region_InitTypeDef r = {0};
    HAL_MPU_Disable();

    r.Enable = MPU_REGION_ENABLE;
    r.Number = MPU_REGION_NUMBER0;
    r.BaseAddress = 0x00000000;
    r.Size = MPU_REGION_SIZE_4GB;
    r.SubRegionDisable = 0x87;
    r.TypeExtField = MPU_TEX_LEVEL0;
    r.AccessPermission = MPU_REGION_NO_ACCESS;
    r.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
    r.IsShareable = MPU_ACCESS_SHAREABLE;
    r.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
    r.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
    HAL_MPU_ConfigRegion(&r);

    r.Number = MPU_REGION_NUMBER1;
    r.BaseAddress = 0x30020000;
    r.Size = MPU_REGION_SIZE_128KB;
    r.SubRegionDisable = 0;
    r.TypeExtField = MPU_TEX_LEVEL1;
    r.AccessPermission = MPU_REGION_FULL_ACCESS;
    r.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
    r.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
    r.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
    r.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
    HAL_MPU_ConfigRegion(&r);

    r.Number = MPU_REGION_NUMBER2;
    r.BaseAddress = 0x30020000;
    r.Size = MPU_REGION_SIZE_512B;
    r.TypeExtField = MPU_TEX_LEVEL0;
    r.IsShareable = MPU_ACCESS_SHAREABLE;
    r.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
    r.IsBufferable = MPU_ACCESS_BUFFERABLE;
    HAL_MPU_ConfigRegion(&r);

    HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
}

static void gpio_config(void)
{
    GPIO_InitTypeDef g = {0};
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();

    HAL_GPIO_WritePin(LED_PORT, LED_PIN, LED_ON_LEVEL == GPIO_PIN_SET ? GPIO_PIN_RESET : GPIO_PIN_SET);
    g.Pin = LED_PIN;
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(LED_PORT, &g);

    g.Pin = KEY_PIN;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = KEY_PULL;
    HAL_GPIO_Init(KEY_PORT, &g);
}

static void uart_config(void)
{
    GPIO_InitTypeDef g = {0};
    __HAL_RCC_USART1_CLK_ENABLE();
    g.Pin = GPIO_PIN_9 | GPIO_PIN_10;
    g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_PULLUP;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    g.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(GPIOA, &g);

    huart_log.Instance = LOG_UART;
    huart_log.Init.BaudRate = LOG_UART_BAUD;
    huart_log.Init.WordLength = UART_WORDLENGTH_8B;
    huart_log.Init.StopBits = UART_STOPBITS_1;
    huart_log.Init.Parity = UART_PARITY_NONE;
    huart_log.Init.Mode = UART_MODE_TX_RX;
    huart_log.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart_log.Init.OverSampling = UART_OVERSAMPLING_16;
    HAL_UART_Init(&huart_log);
}

int main(void)
{
    mpu_config();
    SCB_EnableICache();
    SCB_EnableDCache();
    HAL_Init();
    clock_config();
    __HAL_RCC_D2SRAM1_CLK_ENABLE();
    __HAL_RCC_D2SRAM2_CLK_ENABLE();
    __HAL_RCC_D2SRAM3_CLK_ENABLE();

    gpio_config();
    uart_config();
    plat_log("\r\nMCA MCU 终端 v1.0.0，SYSCLK %lu MHz\n", (unsigned long)(HAL_RCC_GetSysClockFreq() / 1000000u));

    lcd_init();
    app_init();          /* 读配置、画首屏 */
    net_init();          /* 用配置里的 IP 起 LwIP */

    uint32_t t_led = 0;
    for (;;) {
        net_poll();
        app_poll();
        uint32_t now = HAL_GetTick();
        if (now - t_led >= 500) {      /* 心跳灯：链路通时慢闪 */
            t_led = now;
            if (plat_link_speed()) HAL_GPIO_TogglePin(LED_PORT, LED_PIN);
            else plat_led(0);
        }
    }
}
