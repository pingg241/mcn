/* 能谱存储：槽 1.. 各存一条能谱及采集统计（槽 0 为配置） */
#ifndef STORE_H
#define STORE_H

#include <stdint.h>

#include "mca_proto.h"

#define STORE_MAGIC   0x4D435350u /* "MCSP" */
#define STORE_VERSION 2u

typedef struct {
    uint32_t magic, version;
    uint32_t nch, shift;
    uint64_t total, overflow;
    mca_mcst_t st;                /* 保存时的最近心跳（活时间、计数） */
    uint64_t counts_at_st, events_at_st;   /* 该心跳时刻的进谱计数与收到的事件数（算计数率、丢失补偿） */
    float    cal[3];
    char     unit[8];
    char     name[16];
    uint32_t crc;                 /* 头（crc 置 0）+ 计数 */
} store_hdr_t;

#define STORE_HDR_SIZE 192        /* 头按 32 字节对齐占位 */

unsigned store_slots(void);       /* 可用能谱槽数（槽号 1..n） */
int store_save(unsigned slot, const char *name);
int store_load(unsigned slot);    /* 调出后 g_acq.frozen = 1 */
int store_info(unsigned slot, store_hdr_t *h);   /* 0 表示槽内有有效能谱 */

#endif
