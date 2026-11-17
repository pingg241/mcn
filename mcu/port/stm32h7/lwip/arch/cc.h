/* LwIP 编译器/平台适配（arm-none-eabi-gcc + newlib） */
#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stdlib.h>

#define LWIP_TIMEVAL_PRIVATE 0
#define BYTE_ORDER LITTLE_ENDIAN
#define LWIP_RAND() ((u32_t)rand())

#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_STRUCT __attribute__((__packed__))
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x

void plat_log(const char *fmt, ...);
#define LWIP_PLATFORM_DIAG(x) do { plat_log x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { plat_log("lwip assert: %s (%s:%d)\n", x, __FILE__, __LINE__); } while (0)

#endif
