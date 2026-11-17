/* STM32H743VI：程序放 Bank 1（1 MB），Bank 2 留作存储槽（board.h NV_*）。
 *   DTCM 128 KB      栈（CPU 独占，DMA 访问不到，所以 DMA 缓冲不能放栈上）
 *   AXI SRAM 512 KB  .data/.bss（含 300 KB 帧缓冲）+ 堆
 *   SRAM1 128 KB     .sram1_bss：寻峰工作区（PLAT_BIG_BSS）
 *   SRAM2 128 KB     以太网描述符、接收缓冲、LwIP 堆；MPU 设为不可缓存（main.c）
 */
ENTRY(Reset_Handler)

MEMORY
{
  FLASH   (rx)  : ORIGIN = 0x08000000, LENGTH = 1024K
  DTCM    (rw)  : ORIGIN = 0x20000000, LENGTH = 128K
  AXISRAM (rw)  : ORIGIN = 0x24000000, LENGTH = 512K
  SRAM1   (rw)  : ORIGIN = 0x30000000, LENGTH = 128K
  SRAM2   (rw)  : ORIGIN = 0x30020000, LENGTH = 128K
  SRAM3   (rw)  : ORIGIN = 0x30040000, LENGTH = 32K
}

_estack = ORIGIN(DTCM) + LENGTH(DTCM);
_Min_Stack_Size = 0x8000;
_Min_Heap_Size  = 0x8000;

SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.glue_7)
    *(.glue_7t)
    *(.eh_frame)
    KEEP (*(.init))
    KEEP (*(.fini))
    . = ALIGN(4);
    _etext = .;
  } >FLASH

  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } >FLASH

  .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH
  .ARM :
  {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } >FLASH

  .preinit_array :
  {
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
  } >FLASH
  .init_array :
  {
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
  } >FLASH
  .fini_array :
  {
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
  } >FLASH

  _sidata = LOADADDR(.data);
  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } >AXISRAM AT> FLASH

  .bss :
  {
    . = ALIGN(4);
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } >AXISRAM

  /* 堆：.bss 之后到 AXI SRAM 末尾（syscalls.c 的 _sbrk），这里只检查至少留出 _Min_Heap_Size */
  ._heap (NOLOAD) :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    _heap_start = .;
    . = . + _Min_Heap_Size;
  } >AXISRAM
  _heap_end = ORIGIN(AXISRAM) + LENGTH(AXISRAM);

  ._stack (NOLOAD) :
  {
    . = ALIGN(8);
    . = . + _Min_Stack_Size;
  } >DTCM

  .sram1_bss (NOLOAD) :
  {
    . = ALIGN(32);
    *(.sram1_bss)
    *(.sram1_bss*)
  } >SRAM1

  /* SRAM2 起始 512 B 给描述符（MPU 设为 Device），其后为不可缓存的普通内存 */
  .eth_desc (NOLOAD) :
  {
    *(.eth_desc)
    ASSERT(. <= ORIGIN(SRAM2) + 512, "ETH 描述符超过 512 B 的 MPU 区");
    . = ORIGIN(SRAM2) + 512;
  } >SRAM2
  .eth_nocache (NOLOAD) :
  {
    . = ALIGN(32);
    *(.eth_nocache)
    *(.eth_nocache*)
  } >SRAM2

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
