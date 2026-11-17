#include "wave.h"

#include <string.h>

#include "fpga_link.h"
#include "platform.h"

wave_t g_wave;

static struct {
    int16_t  s[WAVE_MAX];
    uint16_t total, got, pretrig;
    uint8_t  src;
    uint32_t capture_id, trig_ts;
    int      active;
} stage;

static void req_cb(void *ctx, uint8_t st, uint32_t v)
{
    (void)ctx; (void)v;
    if (st != MCA_ST_OK) {
        g_wave.pending = 0;
        plat_log("wave: CAPTURE_WAVE 失败 %s\n", mca_status_name(st));
    }
}

int wave_request(uint8_t src, uint8_t depth_code, uint16_t pretrig, int force)
{
    if (depth_code > 4 || src > 3) return -1;
    uint16_t total = (uint16_t)(256u << depth_code);
    if (pretrig >= total) return -1;
    uint32_t addr = ((uint32_t)(src | (force ? 0x80u : 0u)) << 24) | ((uint32_t)depth_code << 16) | pretrig;
    if (fl_cmd(MCA_CMD_CAPTURE_WAVE, addr, 0, req_cb, 0)) return -1;
    g_wave.pending   = 1;
    g_wave.timed_out = 0;
    g_wave.t_req_ms  = plat_now_ms();
    stage.active = 0;
    return 0;
}

void wave_on_mcwf(const mca_mcwf_t *w)
{
    if (w->total > WAVE_MAX || w->offset + w->n > w->total) return;
    if (w->offset == 0 || !stage.active || w->capture_id != stage.capture_id) {
        if (w->offset != 0) { stage.active = 0; return; }    /* 首包丢了，整条作废 */
        stage.capture_id = w->capture_id;
        stage.total   = w->total;
        stage.pretrig = w->pretrig;
        stage.src     = w->src & 3u;
        stage.trig_ts = w->trig_ts;
        stage.got     = 0;
        stage.active  = 1;
    }
    for (unsigned i = 0; i < w->n; i++) stage.s[w->offset + i] = mca_mcwf_sample(w, i);
    stage.got = (uint16_t)(stage.got + w->n);
    if (stage.got >= stage.total) {
        memcpy(g_wave.s, stage.s, sizeof(int16_t) * stage.total);
        g_wave.total      = stage.total;
        g_wave.pretrig    = stage.pretrig;
        g_wave.src        = stage.src;
        g_wave.trig_ts    = stage.trig_ts;
        g_wave.capture_id = stage.capture_id;
        g_wave.count++;
        g_wave.pending = 0;
        stage.active   = 0;
    }
}

void wave_poll(uint32_t now)
{
    /* 没有脉冲触发时 5 s 后放弃 */
    if (g_wave.pending && now - g_wave.t_req_ms > 5000) {
        g_wave.pending   = 0;
        g_wave.timed_out = 1;
    }
}

const char *wave_src_name(unsigned src)
{
    static const char *const names[4] = {"RAW", "POL", "FAST", "SLOW"};
    return names[src & 3u];
}
