#include "app.h"

#include <string.h>

#include "acq.h"
#include "console.h"
#include "fpga_link.h"
#include "gfx.h"
#include "mca_proto.h"
#include "platform.h"
#include "settings.h"
#include "spectrum.h"
#include "ui.h"
#include "wave.h"

#define ANALYZE_PERIOD 2000   /* ms */

static double   preset_live;
static uint32_t t_analyze;
static uint64_t analyzed_total = ~0ull;

void app_on_fpga_cleared(void)
{
    acq_clear();
    spec_reset(g_set.nch, settings_shift());
    analyzed_total = ~0ull;
}

void app_start(double preset_live_s)
{
    if (g_acq.frozen) app_clear();   /* 调出的谱不能接着累加 */
    preset_live = preset_live_s > 0 ? preset_live_s : 0;
    fl_set_run(1);
}

void app_stop(void) { fl_set_run(0); }

void app_clear(void) { fl_clear(); }

double app_preset(void) { return preset_live; }

void app_reanalyze(void) { analyzed_total = ~0ull; t_analyze = plat_now_ms() - ANALYZE_PERIOD; }

static int is_one_of(const char *key, const char *const *list)
{
    for (; *list; list++)
        if (strcmp(key, *list) == 0) return 1;
    return 0;
}

void app_setting_changed(const char *key)
{
    static const char *const fpga_keys[] = {"tau_us", "rise_us", "flat_us", "fast_us", "gap_us", "thr_lsb",
                                            "polarity_neg", "pu_discard", "test_src", "tp_rate", "tp_amp",
                                            "stream_n", "stream_ms", 0};
    static const char *const cal_keys[] = {"cal0", "cal1", "cal2", "cal_unit", 0};
    int all = strcmp(key, "*") == 0;
    if (all || strcmp(key, "fpga_ip") == 0) {
        int run = fl_run_wanted();
        fl_init();                   /* 换目标地址：重新发现 */
        fl_set_run(run);
        app_on_fpga_cleared();
    } else if (is_one_of(key, fpga_keys)) {
        if (fl_state() != FL_DISCOVER) fl_reconfigure();   /* 含清计数器，完成后清谱 */
        else app_on_fpga_cleared();
    } else if (strcmp(key, "nch") == 0) {
        app_clear();
    } else if (is_one_of(key, cal_keys)) {
        app_reanalyze();
    }
}

void app_udp_rx(uint16_t local_port, uint32_t src_ip, uint16_t src_port, const uint8_t *data, size_t len)
{
    if (local_port == MCA_PORT_CONSOLE) {
        console_rx(src_ip, src_port, data, len);
        return;
    }
    if (src_ip != g_set.fpga_ip) return;
    switch (mca_pkt_type(data, len)) {
    case MCA_PKT_MCRS: {
        mca_mcrs_t r;
        if (mca_parse_mcrs(data, len, &r) == 0) fl_on_mcrs(&r);
        break;
    }
    case MCA_PKT_MCST: {
        mca_mcst_t s;
        if (mca_parse_mcst(data, len, &s) == 0) {
            fl_on_mcst(&s);
            acq_on_mcst(&s);
        }
        break;
    }
    case MCA_PKT_MCEV: {
        mca_mcev_t m;
        if (mca_parse_mcev(data, len, &m) == 0) acq_on_mcev(&m, g_set.keep_pileup);
        break;
    }
    case MCA_PKT_MCWF: {
        mca_mcwf_t w;
        if (mca_parse_mcwf(data, len, &w) == 0) wave_on_mcwf(&w);
        break;
    }
    default:
        break;
    }
}

void app_init(void)
{
    if (settings_load() != 0) plat_log("app: 没有有效配置，使用默认值\n");
    gfx_init(plat_framebuffer());
    spec_reset(g_set.nch, settings_shift());
    acq_clear();
    fl_init();
    console_init();
    ui_init();
    t_analyze = plat_now_ms();
}

void app_poll(void)
{
    uint32_t now = plat_now_ms();
    fl_poll(now);
    wave_poll(now);
    console_poll(now);

    if (preset_live > 0 && fl_running() && !g_acq.frozen) {
        acq_stats_t s;
        acq_stats(&s);
        if (s.live_s >= preset_live) {
            plat_log("app: 活时间到达预置 %.1f s，停止\n", preset_live);
            preset_live = 0;
            app_stop();
        }
    }

    if (now - t_analyze >= ANALYZE_PERIOD) {
        t_analyze = now;
        if (g_spec.total != analyzed_total) {
            analyzed_total = g_spec.total;
            spec_analyze();
        }
    }

    ui_poll(now);
}
