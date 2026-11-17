#include "net.h"

#include <string.h>

#include "app.h"
#include "board.h"
#include "lwip/etharp.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/timeouts.h"
#include "lwip/udp.h"
#include "mca_proto.h"
#include "netif/ethernet.h"
#include "platform.h"
#include "settings.h"

#define RX_BUF_SIZE    1536
#define RX_BUF_CNT     ((int)ETH_RX_DESC_CNT + 4)
#define LINK_POLL_MS   200

ETH_HandleTypeDef heth;
static ETH_DMADescTypeDef rx_desc[ETH_RX_DESC_CNT] __attribute__((section(".eth_desc")));
static ETH_DMADescTypeDef tx_desc[ETH_TX_DESC_CNT] __attribute__((section(".eth_desc")));
static uint8_t rx_buf[RX_BUF_CNT][RX_BUF_SIZE] __attribute__((section(".eth_nocache"), aligned(32)));
static uint8_t rx_used[RX_BUF_CNT];
/* LwIP 堆（lwipopts.h 的 LWIP_RAM_HEAP_POINTER）：UDP 发送的 pbuf 在这里，DMA 直接读 */
uint8_t lwip_ram_heap[MEM_SIZE + 64] __attribute__((section(".eth_nocache"), aligned(32)));

static ETH_TxPacketConfigTypeDef tx_cfg;
static struct netif nif;
static struct udp_pcb *pcbs[3];
static const uint16_t ports[3] = {MCA_PORT_CMD, MCA_PORT_EV, MCA_PORT_CONSOLE};
static int eth_ok, link_mbps;
static uint32_t phy_addr, t_link;

/* ---------------- PHY（通用 IEEE 802.3 寄存器，LAN8720/DP83848/RTL8201 都可） ---------------- */
static uint32_t phy_rd(uint32_t reg)
{
    uint32_t v = 0xFFFF;
    HAL_ETH_ReadPHYRegister(&heth, phy_addr, reg, &v);
    return v;
}
static void phy_wr(uint32_t reg, uint32_t v) { HAL_ETH_WritePHYRegister(&heth, phy_addr, reg, v); }

static int phy_init(void)
{
    for (phy_addr = 0; phy_addr < 32; phy_addr++) {
        uint32_t id1 = phy_rd(2);
        if (id1 != 0xFFFF && id1 != 0) break;
    }
    if (phy_addr == 32) {
        plat_log("net: 找不到 PHY\n");
        return -1;
    }
    plat_log("net: PHY 地址 %lu，ID %04lX:%04lX\n", (unsigned long)phy_addr, (unsigned long)phy_rd(2),
             (unsigned long)phy_rd(3));
    phy_wr(0, 0x8000);                       /* 软复位 */
    uint32_t t0 = HAL_GetTick();
    while ((phy_rd(0) & 0x8000) && HAL_GetTick() - t0 < 500) {}
    phy_wr(4, 0x01E1);                       /* 通告 10/100 全/半双工 */
    phy_wr(0, 0x1200);                       /* 自协商使能并重启 */
    return 0;
}

/* 返回 0 断开，否则 10/100；*fd 全双工 */
static int phy_link(int *fd)
{
    uint32_t bsr = phy_rd(1);
    bsr = phy_rd(1);                         /* 链路位是锁存低，读两次 */
    if (!(bsr & 0x0004) || !(bsr & 0x0020)) return 0;    /* 未连通或自协商未完成 */
    uint32_t common = phy_rd(4) & phy_rd(5);
    if (common & 0x0100) { *fd = 1; return 100; }
    if (common & 0x0080) { *fd = 0; return 100; }
    if (common & 0x0040) { *fd = 1; return 10; }
    *fd = 0;
    return 10;
}

/* ---------------- HAL 回调 ---------------- */
void HAL_ETH_MspInit(ETH_HandleTypeDef *h)
{
    (void)h;
    GPIO_InitTypeDef g = {0};
    __HAL_RCC_ETH1MAC_CLK_ENABLE();
    __HAL_RCC_ETH1TX_CLK_ENABLE();
    __HAL_RCC_ETH1RX_CLK_ENABLE();
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF11_ETH;
    g.Pin = GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_7;
    HAL_GPIO_Init(GPIOA, &g);
    g.Pin = GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13;
    HAL_GPIO_Init(GPIOB, &g);
    g.Pin = GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5;
    HAL_GPIO_Init(GPIOC, &g);
}

/* 接收缓冲：DMA 写入后在 RxLinkCallback 里拷到 PBUF_POOL，缓冲立即归还 */
void HAL_ETH_RxAllocateCallback(uint8_t **buff)
{
    for (int i = 0; i < RX_BUF_CNT; i++)
        if (!rx_used[i]) {
            rx_used[i] = 1;
            *buff = rx_buf[i];
            return;
        }
    *buff = NULL;
}

void HAL_ETH_RxLinkCallback(void **pStart, void **pEnd, uint8_t *buff, uint16_t len)
{
    int idx = (int)((buff - &rx_buf[0][0]) / RX_BUF_SIZE);
    struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
    if (p) pbuf_take(p, buff, len);
    if (idx >= 0 && idx < RX_BUF_CNT) rx_used[idx] = 0;
    if (!p) return;
    if (!*pStart) *pStart = p;
    else pbuf_cat((struct pbuf *)*pStart, p);
    *pEnd = p;
}

/* ---------------- netif ---------------- */
static err_t low_level_output(struct netif *n, struct pbuf *p)
{
    (void)n;
    ETH_BufferTypeDef tb[ETH_TX_DESC_CNT];
    int i = 0;
    memset(tb, 0, sizeof(tb));
    for (struct pbuf *q = p; q; q = q->next) {
        if (i >= (int)ETH_TX_DESC_CNT) return ERR_IF;
        tb[i].buffer = q->payload;
        tb[i].len = q->len;
        if (i) tb[i - 1].next = &tb[i];
        /* RX 回包（ICMP 回显）等 pbuf 可能在可缓存内存里，发送前写回 */
        uintptr_t a = (uintptr_t)q->payload & ~31u;
        SCB_CleanDCache_by_Addr((uint32_t *)a, (int32_t)(((uintptr_t)q->payload + q->len - a + 31u) & ~31u));
        i++;
    }
    tx_cfg.Length = p->tot_len;
    tx_cfg.TxBuffer = tb;
    tx_cfg.pData = p;
    /* 阻塞式 Transmit 等到该帧最后一个描述符的 OWN 清零才返回，返回时 DMA 已读完 pbuf，所以不用 pbuf_ref。
     * 它不记录 pData（只有 _IT 版本记录），也不要调 HAL_ETH_ReleaseTxPacket：那样 BuffersInUse
     * 只增不减，每次释放都要从头扫一遍 */
    HAL_StatusTypeDef st = HAL_ETH_Transmit(&heth, &tx_cfg, 20);
    return st == HAL_OK ? ERR_OK : ERR_IF;
}

static err_t netif_setup(struct netif *n)
{
    n->name[0] = 'e';
    n->name[1] = '0';
    n->output = etharp_output;
    n->linkoutput = low_level_output;
    n->mtu = 1500;
    n->hwaddr_len = ETH_HWADDR_LEN;
    memcpy(n->hwaddr, heth.Init.MACAddr, 6);
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    return ERR_OK;
}

/* ---------------- UDP ---------------- */
static void udp_rx(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    static uint8_t buf[1536];
    (void)arg;
    uint16_t n = pbuf_copy_partial(p, buf, sizeof(buf), 0);
    app_udp_rx(pcb->local_port, lwip_ntohl(ip4_addr_get_u32(ip_2_ip4(addr))), port, buf, n);
    pbuf_free(p);
}

int plat_udp_send(uint16_t local_port, uint32_t dst_ip, uint16_t dst_port, const void *data, size_t len)
{
    if (!link_mbps || len > 1472) return -1;
    for (int i = 0; i < 3; i++) {
        if (ports[i] != local_port || !pcbs[i]) continue;
        struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
        if (!p) return -1;
        memcpy(p->payload, data, len);
        ip_addr_t dst;
        ip_addr_set_ip4_u32(&dst, lwip_htonl(dst_ip));
        err_t e = udp_sendto(pcbs[i], p, &dst, dst_port);
        pbuf_free(p);
        return e == ERR_OK ? 0 : -1;
    }
    return -1;
}

int plat_link_speed(void) { return link_mbps; }
uint32_t plat_local_ip(void) { return g_set.mcu_ip; }
u32_t sys_now(void) { return HAL_GetTick(); }

/* ---------------- 初始化与轮询 ---------------- */
static void make_mac(uint8_t mac[6])
{
    /* 本地管理地址 02:4D:43 + 芯片 UID 散列 */
    uint32_t h = mca_crc32(0, (const void *)UID_BASE, 12);
    mac[0] = 0x02; mac[1] = 0x4D; mac[2] = 0x43;
    mac[3] = (uint8_t)(h >> 16); mac[4] = (uint8_t)(h >> 8); mac[5] = (uint8_t)h;
}

void net_init(void)
{
    static uint8_t mac[6];
    make_mac(mac);
    heth.Instance = ETH;
    heth.Init.MACAddr = mac;
    heth.Init.MediaInterface = HAL_ETH_RMII_MODE;
    heth.Init.TxDesc = tx_desc;
    heth.Init.RxDesc = rx_desc;
    heth.Init.RxBuffLen = RX_BUF_SIZE;
    if (HAL_ETH_Init(&heth) != HAL_OK) {
        /* 多半是没接 PHY：RMII 50 MHz 参考时钟不在，MAC DMA 复位不完 */
        plat_log("net: HAL_ETH_Init 失败（PHY 参考时钟？），网络不可用\n");
        return;
    }
    memset(&tx_cfg, 0, sizeof(tx_cfg));
    tx_cfg.Attributes = ETH_TX_PACKETS_FEATURES_CRCPAD;
    tx_cfg.CRCPadCtrl = ETH_CRC_PAD_INSERT;
    tx_cfg.ChecksumCtrl = ETH_CHECKSUM_DISABLE;     /* 校验和由 LwIP 软件计算 */
    if (phy_init()) return;

    lwip_init();
    ip4_addr_t ip, mask, gw;
    ip4_addr_set_u32(&ip, lwip_htonl(g_set.mcu_ip));
    ip4_addr_set_u32(&mask, lwip_htonl(g_set.netmask));
    ip4_addr_set_u32(&gw, lwip_htonl(g_set.gateway));
    netif_add(&nif, &ip, &mask, &gw, NULL, netif_setup, ethernet_input);
    netif_set_default(&nif);
    netif_set_up(&nif);
    for (int i = 0; i < 3; i++) {
        pcbs[i] = udp_new();
        if (!pcbs[i] || udp_bind(pcbs[i], IP_ADDR_ANY, ports[i]) != ERR_OK) {
            plat_log("net: UDP %u 绑定失败\n", ports[i]);
            continue;
        }
        udp_recv(pcbs[i], udp_rx, NULL);
    }
    eth_ok = 1;
    plat_log("net: MAC %02X:%02X:%02X:%02X:%02X:%02X IP %u.%u.%u.%u\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             (unsigned)(g_set.mcu_ip >> 24), (unsigned)(g_set.mcu_ip >> 16) & 255, (unsigned)(g_set.mcu_ip >> 8) & 255,
             (unsigned)g_set.mcu_ip & 255);
}

static void link_poll(uint32_t now)
{
    if (now - t_link < LINK_POLL_MS) return;
    t_link = now;
    int fd = 0, sp = phy_link(&fd);
    if (sp && !link_mbps) {
        ETH_MACConfigTypeDef mc;
        HAL_ETH_GetMACConfig(&heth, &mc);
        mc.Speed = sp == 100 ? ETH_SPEED_100M : ETH_SPEED_10M;
        mc.DuplexMode = fd ? ETH_FULLDUPLEX_MODE : ETH_HALFDUPLEX_MODE;
        HAL_ETH_SetMACConfig(&heth, &mc);
        HAL_ETH_Start(&heth);
        netif_set_link_up(&nif);
        link_mbps = sp;
        plat_log("net: 链路 %dM %s\n", sp, fd ? "全双工" : "半双工");
    } else if (!sp && link_mbps) {
        HAL_ETH_Stop(&heth);
        netif_set_link_down(&nif);
        link_mbps = 0;
        plat_log("net: 链路断开\n");
    }
}

void net_poll(void)
{
    if (!eth_ok) return;
    uint32_t now = HAL_GetTick();
    link_poll(now);
    if (link_mbps) {
        for (int k = 0; k < (int)ETH_RX_DESC_CNT; k++) {
            struct pbuf *p = NULL;
            if (HAL_ETH_ReadData(&heth, (void **)&p) != HAL_OK || !p) break;
            if (nif.input(p, &nif) != ERR_OK) pbuf_free(p);
        }
    }
    sys_check_timeouts();
}
