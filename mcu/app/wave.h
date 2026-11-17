/* 波形捕获：发 CAPTURE_WAVE，重组 MCWF 分包（收齐后整体换入 g_wave.s） */
#ifndef WAVE_H
#define WAVE_H

#include <stdint.h>

#include "mca_proto.h"

#define WAVE_MAX 4096

typedef struct {
    int16_t  s[WAVE_MAX];      /* 最近一次完整波形 */
    uint16_t total, pretrig;
    uint8_t  src;
    uint32_t capture_id, trig_ts;
    uint32_t count;            /* 已收齐的波形数，变化即有新波形 */
    int      pending;          /* 已请求、尚未收齐 */
    int      timed_out;        /* 最近一次请求超时（没有触发） */
    uint32_t t_req_ms;
} wave_t;

extern wave_t g_wave;

/* src 0 原始 1 极性校正 2 快滤波 3 慢滤波；depth_code 0..4（256..4096 点）；force 不等触发 */
int  wave_request(uint8_t src, uint8_t depth_code, uint16_t pretrig, int force);
void wave_on_mcwf(const mca_mcwf_t *w);
void wave_poll(uint32_t now);
const char *wave_src_name(unsigned src);

#endif
