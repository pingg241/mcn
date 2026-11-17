/* 采集统计：事件进谱；包序号空洞 + 心跳 pkt_seq 查丢包；心跳换算活时间/死时间/计数率；
 * 心跳 total_events 对照收到的事件数做丢失补偿 */
#ifndef ACQ_H
#define ACQ_H

#include <stdint.h>

#include "mca_proto.h"

typedef struct {
    uint64_t events;           /* 收到的事件 */
    uint64_t pkts;             /* 收到的 MCEV 包 */
    uint64_t flag_cnt[4];      /* 饱和 / 基线 / 测试 / 堆积 */
    uint64_t dup_pkts;         /* 重复或乱序包 */
    uint32_t first_seq, max_seq;
    int      have_seq;
    mca_mcst_t st;             /* 最新心跳 */
    int      have_st;
    /* 收到该心跳时的进谱计数与收到的事件数：计数率、丢失补偿要和心跳里的时间、计数对齐 */
    uint64_t counts_at_st, events_at_st;
    uint32_t t_clear_ms;
    uint64_t last_ts;          /* 最近事件时间戳 */
    int      frozen;           /* 显示的是存储区调出的谱：忽略事件与心跳 */
} acq_t;

typedef struct {
    double   real_s, live_s, dead_pct;
    double   icr;              /* 输入计数率：FPGA 触发数 / 实时间 */
    double   ocr;              /* 输出计数率：进谱计数（补偿后）/ 实时间 */
    double   rate;             /* 死时间校正计数率：进谱计数（补偿后）/ 活时间 */
    double   loss_factor;      /* FPGA 应发出的事件 / 收到的事件 */
    double   lost_pkt_pct;
    uint64_t lost_pkts;
    uint64_t lost_events;      /* 应发出 − 收到（含 FIFO 满丢弃与 UDP 丢包） */
    uint64_t counts;           /* 谱中计数 */
    uint64_t fpga_events, fpga_pileup, fpga_dropped, fpga_sat;
} acq_stats_t;

extern acq_t g_acq;

void acq_clear(void);
void acq_on_mcev(const mca_mcev_t *m, int keep_pileup);
void acq_on_mcst(const mca_mcst_t *s);
void acq_stats(acq_stats_t *o);

#endif
