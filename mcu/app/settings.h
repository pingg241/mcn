/* MCU 配置项：默认值、校验、存取 */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>

#define SETTINGS_MAGIC   0x4D435553u   /* "MCUS" */
#define SETTINGS_VERSION 1u

typedef struct {
    uint32_t magic, version;

    /* 网络（IP 按 a.b.c.d = 0xAABBCCDD） */
    uint32_t fpga_ip, mcu_ip, netmask, gateway;

    /* 前端滤波（物理量，由 filter_cfg 换算成寄存器） */
    float tau_us;        /* 前放衰减时间常数 */
    float rise_us;       /* 梯形上升时间 */
    float flat_us;       /* 平顶 */
    float fast_us;       /* 快滤波长度 */
    float gap_us;        /* 快滤波间隔 */
    float thr_lsb;       /* 触发阈值（脉冲幅度 LSB） */
    uint8_t polarity_neg, pu_discard, test_src, keep_pileup;
    float tp_rate;       /* 测试源计数率 cps */
    float tp_amp;        /* 测试源幅度 LSB */

    /* 能谱 */
    uint16_t nch;        /* 道数 1024/2048/4096/8192（道址 = energy >> (16 - log2 nch)） */
    uint8_t  log_scale;
    uint8_t  stream_n;   /* FPGA 每包事件数 */
    uint16_t stream_ms;  /* FPGA 发包超时 */
    uint16_t reserved;
    float cal[3];        /* E = c0 + c1·ch + c2·ch² */
    char  cal_unit[8];   /* "keV" 或 "ch" */

    uint32_t crc;
} settings_t;

extern settings_t g_set;

void settings_defaults(settings_t *s);
int  settings_load(void);          /* 从槽 0 读取，失败则用默认值；返回 0 表示读到有效配置 */
int  settings_save(void);
uint32_t settings_crc(const settings_t *s);

/* 文本接口：key value，返回 0 成功 */
int settings_set(const char *key, const char *value);
int settings_get(const char *key, char *out, int n);
/* 列出全部 key=value，每行一个 */
int settings_dump(char *out, int n);

unsigned settings_shift(void);     /* energy → 道址右移位数 */

#endif
