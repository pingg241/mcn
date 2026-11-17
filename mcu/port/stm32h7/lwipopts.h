/* LwIP 配置：裸机（NO_SYS），只要 ARP/IPv4/ICMP/UDP */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#define NO_SYS                      1
#define SYS_LIGHTWEIGHT_PROT        0     /* 只在主循环里调用 LwIP */
#define LWIP_NETCONN                0
#define LWIP_SOCKET                 0
#define LWIP_TCP                    0
#define LWIP_UDP                    1
#define LWIP_RAW                    0
#define LWIP_ICMP                   1     /* 能 ping 通方便调试 */
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_DHCP                   0
#define LWIP_AUTOIP                 0
#define LWIP_DNS                    0
#define LWIP_IGMP                   0
#define LWIP_ARP                    1
#define ARP_QUEUEING                1     /* ARP 解析期间先缓存第一个包 */
#define LWIP_SINGLE_NETIF           1
#define LWIP_NETIF_LINK_CALLBACK    0
#define LWIP_NETIF_STATUS_CALLBACK  0
#define IP_REASSEMBLY               0     /* 本协议的数据报都小于 MTU */
#define IP_FRAG                     0

/* 内存：堆放在 SRAM2（不可缓存，net.c 定义），发送 pbuf 由以太网 DMA 直接读 */
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    (16 * 1024)
extern unsigned char lwip_ram_heap[];
#define LWIP_RAM_HEAP_POINTER       lwip_ram_heap
#define MEMP_NUM_PBUF               8
#define MEMP_NUM_UDP_PCB            4
#define MEMP_NUM_ARP_QUEUE          8
#define PBUF_POOL_SIZE              16
#define PBUF_POOL_BUFSIZE           1536  /* 一帧放进一个 pbuf */
#define ETH_PAD_SIZE                0

/* 校验和全部软件算（MAC 不做插入） */
#define CHECKSUM_GEN_IP             1
#define CHECKSUM_GEN_UDP            1
#define CHECKSUM_GEN_ICMP           1
#define CHECKSUM_CHECK_IP           1
#define CHECKSUM_CHECK_UDP          1
#define CHECKSUM_CHECK_ICMP         1

#define LWIP_STATS                  0
#define LWIP_DEBUG                  0

#endif
