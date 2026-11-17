#include "board.h"

extern SPI_HandleTypeDef hspi_lcd;
extern DMA_HandleTypeDef hdma_lcd;
void Error_Handler(void);

void NMI_Handler(void) { for (;;) {} }
void HardFault_Handler(void) { Error_Handler(); }
void MemManage_Handler(void) { Error_Handler(); }
void BusFault_Handler(void) { Error_Handler(); }
void UsageFault_Handler(void) { Error_Handler(); }
void SVC_Handler(void) {}
void DebugMon_Handler(void) {}
void PendSV_Handler(void) {}
void SysTick_Handler(void) { HAL_IncTick(); }

void DMA1_Stream0_IRQHandler(void) { HAL_DMA_IRQHandler(&hdma_lcd); }
void SPI4_IRQHandler(void) { HAL_SPI_IRQHandler(&hspi_lcd); }
