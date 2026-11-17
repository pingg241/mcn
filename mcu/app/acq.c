#include "acq.h"

#include <string.h>

#include "platform.h"
#include "spectrum.h"

acq_t g_acq;

void acq_clear(void)
{
    memset(&g_acq, 0, sizeof(g_acq));
    g_acq.t_clear_ms = plat_now_ms();
}

void acq_on_mcev(const mca_mcev_t *m, int keep_pileup)
{
    acq_t *a = &g_acq;
    if (a->frozen) return;
    if (!a->have_seq) {
        a->first_seq = a->max_seq = m->seq;
        a->have_seq  = 1;
    } else if ((int32_t)(m->seq - a->max_seq) > 0) {
        a->max_seq = m->seq;
    } else {
        a->dup_pkts++;              /* 直连链路上不应出现，计数后丢弃 */
        return;
    }
    a->pkts++;
    for (unsigned i = 0; i < m->n; i++) {
        mca_event_t ev;
        mca_mcev_event(m, i, &ev);
        a->events++;
        if (ev.flags & MCA_EV_SAT)      a->flag_cnt[0]++;
        if (ev.flags & MCA_EV_BASELINE) a->flag_cnt[1]++;
        if (ev.flags & MCA_EV_TEST)     a->flag_cnt[2]++;
        if (ev.flags & MCA_EV_PILEUP) {
            a->flag_cnt[3]++;
            if (!keep_pileup) continue;
        }
        a->last_ts = ev.ts;
        spec_add(ev.energy);
    }
}

void acq_on_mcst(const mca_mcst_t *s)
{
    if (g_acq.frozen) return;
    g_acq.st = *s;
    g_acq.have_st = 1;
    g_acq.counts_at_st = g_spec.total;
    g_acq.events_at_st = g_acq.events;
}

void acq_stats(acq_stats_t *o)
{
    const acq_t *a = &g_acq;
    memset(o, 0, sizeof(*o));
    o->counts = g_spec.total;
    o->loss_factor = 1.0;
    if (a->have_seq) {
        /* 丢包：序号空洞，再用心跳的 pkt_seq（FPGA 已发出的下一个序号）补上末尾丢的包。
         * 心跳在这些包之后发出、同一链路按序到达，所以比较时不会把在途的包算成丢失 */
        uint32_t end = a->max_seq + 1u;
        if (a->have_st && (int32_t)(a->st.pkt_seq - end) > 0) end = a->st.pkt_seq;
        uint64_t span = (uint32_t)(end - a->first_seq);
        o->lost_pkts = span > a->pkts ? span - a->pkts : 0;
        o->lost_pkt_pct = span ? 100.0 * (double)o->lost_pkts / (double)span : 0.0;
    }
    if (!a->have_st) return;
    const mca_mcst_t *s = &a->st;
    o->real_s   = (double)s->total_ticks / MCA_FS_HZ;
    o->live_s   = (double)(s->total_ticks - (s->busy_ticks < s->total_ticks ? s->busy_ticks : s->total_ticks)) / MCA_FS_HZ;
    o->dead_pct = s->total_ticks ? 100.0 * (double)s->busy_ticks / (double)s->total_ticks : 0.0;
    o->icr      = o->real_s > 0 ? (double)s->total_events / o->real_s : 0.0;
    o->fpga_events  = s->total_events;
    o->fpga_pileup  = s->pileup_events;
    o->fpga_dropped = s->dropped_events;
    o->fpga_sat     = s->adc_sat_events;
    /* 丢失补偿：FPGA 应发出的事件 = 触发总数 − PU_DISCARD 丢掉的堆积事件，FIFO 满丢弃和 UDP 丢包都包含在
     * 与"收到"的差里。两边都取心跳时刻；心跳时还在 FPGA 打包缓冲里的事件（不超过一包或 1 ms）会暂算作丢失。
     * 丢失是随机的，对各道等比例补偿，峰位和 FWHM 不受影响 */
    uint64_t expect = s->total_events;
    if (s->status_flags & MCA_SF_PU_DISCARD) expect -= s->pileup_events < expect ? s->pileup_events : expect;
    if (a->events_at_st && expect > a->events_at_st) {
        o->lost_events = expect - a->events_at_st;
        o->loss_factor = (double)expect / (double)a->events_at_st;
    }
    /* 心跳最多晚 1 s，用心跳时刻的计数，否则计数率偏高 */
    double c = (double)a->counts_at_st * o->loss_factor;
    o->ocr  = o->real_s > 0 ? c / o->real_s : 0.0;
    o->rate = o->live_s > 0 ? c / o->live_s : 0.0;
}
