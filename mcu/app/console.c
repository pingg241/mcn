#include "console.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "acq.h"
#include "app.h"
#include "fpga_link.h"
#include "mca_proto.h"
#include "platform.h"
#include "settings.h"
#include "spectrum.h"
#include "store.h"
#include "wave.h"

#define OUT_MAX  1400          /* 每个回复数据报的最大长度 */
#define LOG_SIZE 2048
#define MAX_TOK  12

typedef struct {
    uint32_t ip;
    uint16_t port;
    int      used;
    console_out_fn out;        /* 非空：本机终端敲的命令，回复交给它，不走 UDP */
} peer_t;

static struct {
    peer_t   cur;              /* 当前命令的发送者 */
    char     out[OUT_MAX];
    int      olen;
    peer_t   pend[4];          /* 等待 FPGA 应答的 reg 命令 */
    peer_t   wave_peer;
    uint32_t wave_count0;
    char     log[LOG_SIZE];
    uint32_t log_pos;
} C;

/* ---------------- 输出 ---------------- */
static void send_to(const peer_t *p, const char *d, size_t n)
{
    if (p->out) p->out(d, n);
    else plat_udp_send(MCA_PORT_CONSOLE, p->ip, p->port, d, n);
}

static void flush_out(void)
{
    if (C.olen) send_to(&C.cur, C.out, (size_t)C.olen);
    C.olen = 0;
}

static void put_raw(const char *s, size_t n)
{
    while (n) {
        if (C.olen == OUT_MAX) flush_out();
        size_t k = OUT_MAX - (size_t)C.olen;
        if (k > n) k = n;
        memcpy(C.out + C.olen, s, k);
        C.olen += (int)k;
        s += k;
        n -= k;
    }
}

/* 按行输出：一行不跨数据报 */
static void cprintf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    if (C.olen + n > OUT_MAX) flush_out();
    put_raw(buf, (size_t)n);
}

/* 异步应答直接发送，不碰正在拼装的输出缓冲 */
static void reply_to(const peer_t *p, const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    send_to(p, buf, (size_t)n);
}

void console_log(const char *s)
{
    for (; *s; s++) C.log[C.log_pos++ % LOG_SIZE] = *s;
}

/* ---------------- 命令 ---------------- */
static int parse_u32(const char *s, uint32_t *v)
{
    char *end;
    unsigned long x = strtoul(s, &end, 0);
    if (end == s || *end) return -1;
    *v = (uint32_t)x;
    return 0;
}

static int parse_f(const char *s, double *v)
{
    char *end;
    *v = strtod(s, &end);
    return end == s || *end ? -1 : 0;
}

static void reg_cb(void *ctx, uint8_t st, uint32_t v)
{
    peer_t *p = (peer_t *)ctx;
    if (st == MCA_ST_OK) reply_to(p, "0x%08X %u\nOK\n", (unsigned)v, (unsigned)v);
    else reply_to(p, "ERR %s\n", mca_status_name(st));
    p->used = 0;
}

static void cmd_help(void)
{
    static const char *const lines[] = {
        "help                      本表",
        "status                    采集状态与统计",
        "start [live_s]            开始（可选活时间预置）",
        "stop | clear              停止 | 清谱并清 FPGA 计数器",
        "get <key> | set <key> <v> 读写配置（set 后自动重新下发 FPGA）",
        "dump | save | defaults    列出 | 保存到闪存 | 恢复默认",
        "spec [first [count]]      读能谱（SPEC 头 + 每行 16 道）",
        "peaks                     寻峰结果",
        "cal <ch> <E> [<ch> <E>..] 能量刻度（1 点比例，2 点线性，3 点以上二次）",
        "calpk <E1> [E2..]         把能量依次赋给按道址排序的峰后刻度",
        "cal off                   取消刻度",
        "wave <src> [depth [pre [force]]]  捕获波形 src 0..3 depth 0..4",
        "store <slot> [name] | recall <slot> | slots  能谱存取",
        "reg <addr> [value]        读写 FPGA 寄存器",
        "reconfig | log            重新下发配置 | 最近日志",
    };
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) cprintf("%s\n", lines[i]);
    cprintf("OK\n");
}

static void cmd_status(void)
{
    acq_stats_t s;
    uint32_t v = fl_version();
    acq_stats(&s);
    cprintf("state %s\nrun_wanted %d\nrunning %d\nfrozen %d\n", fl_state_name(), fl_run_wanted(), fl_running(),
            g_acq.frozen);
    cprintf("fpga_version %u.%u.%u\nstatus_flags 0x%04X\nlink_mbps %d\n", (unsigned)(v >> 16),
            (unsigned)(v >> 8) & 255, (unsigned)v & 255, (unsigned)fl_last_status(), plat_link_speed());
    cprintf("real_s %.3f\nlive_s %.3f\ndead_pct %.3f\npreset_live_s %.1f\n", s.real_s, s.live_s, s.dead_pct,
            app_preset());
    cprintf("icr %.2f\nocr %.2f\nrate %.2f\n", s.icr, s.ocr, s.rate);
    cprintf("counts %llu\noverflow %llu\nevents_rx %llu\npkts_rx %llu\n", (unsigned long long)s.counts,
            (unsigned long long)g_spec.overflow, (unsigned long long)g_acq.events, (unsigned long long)g_acq.pkts);
    cprintf("fpga_events %llu\nfpga_pileup %llu\nfpga_dropped %llu\nfpga_sat %llu\n",
            (unsigned long long)s.fpga_events, (unsigned long long)s.fpga_pileup,
            (unsigned long long)s.fpga_dropped, (unsigned long long)s.fpga_sat);
    cprintf("flag_sat %llu\nflag_baseline %llu\nflag_test %llu\nflag_pileup %llu\n",
            (unsigned long long)g_acq.flag_cnt[0], (unsigned long long)g_acq.flag_cnt[1],
            (unsigned long long)g_acq.flag_cnt[2], (unsigned long long)g_acq.flag_cnt[3]);
    cprintf("lost_events %llu\n", (unsigned long long)s.lost_events);
    cprintf("lost_pkts %llu\nlost_pkt_pct %.3f\ndup_pkts %llu\nloss_factor %.5f\ncmd_timeouts %u\npeaks %d\nOK\n",
            (unsigned long long)s.lost_pkts, s.lost_pkt_pct, (unsigned long long)g_acq.dup_pkts, s.loss_factor,
            (unsigned)fl_timeouts(), g_spec.npeaks);
}

static void cmd_spec(int argc, char **argv)
{
    uint32_t first = 0, count = g_spec.nch;
    if ((argc > 1 && parse_u32(argv[1], &first)) || (argc > 2 && parse_u32(argv[2], &count))) {
        cprintf("ERR bad args\n");
        return;
    }
    if (first > g_spec.nch) first = g_spec.nch;
    if (count > g_spec.nch - first) count = g_spec.nch - first;
    cprintf("SPEC %u %u %u %llu\n", (unsigned)first, (unsigned)count, (unsigned)g_spec.nch,
            (unsigned long long)g_spec.total);
    char line[200];
    for (uint32_t i = 0; i < count; i += 16) {
        int k = 0;
        for (uint32_t j = i; j < i + 16 && j < count; j++)
            k += snprintf(line + k, sizeof(line) - (size_t)k, j == i ? "%u" : " %u", (unsigned)g_spec.h[first + j]);
        cprintf("%s\n", line);
    }
    cprintf("OK\n");
}

static void cmd_peaks(void)
{
    cprintf("# centroid sigma fwhm area area_err height energy fwhm_e res_pct chi2ndf\n");
    for (int i = 0; i < g_spec.npeaks; i++) {
        const peak_t *p = &g_spec.peaks[i];
        cprintf("%d %.3f %.3f %.3f %.1f %.1f %.1f %.4f %.4f %.3f %.2f\n", i + 1, p->centroid, p->sigma, p->fwhm,
                p->area, p->area_err, p->height, p->energy, p->fwhm_e, p->res_pct, p->chi2ndf);
    }
    cprintf("OK\n");
}

static void apply_cal(const double *ch, const double *e, int n)
{
    float c[3];
    if (spec_calibrate(ch, e, n, c)) {
        cprintf("ERR calibration failed\n");
        return;
    }
    memcpy(g_set.cal, c, sizeof(c));
    if (strcmp(g_set.cal_unit, "ch") == 0) strcpy(g_set.cal_unit, "keV");
    app_setting_changed("cal0");
    cprintf("cal %.6g %.6g %.6g %s\nOK\n", g_set.cal[0], g_set.cal[1], g_set.cal[2], g_set.cal_unit);
}

static void cmd_cal(int argc, char **argv)
{
    double ch[8], e[8];
    if (argc == 2 && strcmp(argv[1], "off") == 0) {
        g_set.cal[0] = 0; g_set.cal[1] = 1; g_set.cal[2] = 0;
        strcpy(g_set.cal_unit, "ch");
        app_setting_changed("cal0");
        cprintf("OK\n");
        return;
    }
    int n = (argc - 1) / 2;
    if (argc < 3 || (argc - 1) % 2 || n > 8) { cprintf("ERR usage: cal <ch> <E> [<ch> <E> ...]\n"); return; }
    for (int i = 0; i < n; i++)
        if (parse_f(argv[1 + 2 * i], &ch[i]) || parse_f(argv[2 + 2 * i], &e[i])) { cprintf("ERR bad number\n"); return; }
    apply_cal(ch, e, n);
}

static void cmd_calpk(int argc, char **argv)
{
    /* 面积最大的 n 个峰按道址升序，依次对应给出的能量 */
    int n = argc - 1;
    double ch[SPEC_MAX_PEAKS], e[SPEC_MAX_PEAKS];
    if (n < 1 || n > g_spec.npeaks) { cprintf("ERR need 1..%d energies\n", g_spec.npeaks); return; }
    for (int i = 0; i < n; i++) {
        ch[i] = g_spec.peaks[i].centroid;
        if (parse_f(argv[1 + i], &e[i])) { cprintf("ERR bad number\n"); return; }
    }
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && ch[j] < ch[j - 1]; j--) { double t = ch[j]; ch[j] = ch[j - 1]; ch[j - 1] = t; }
    apply_cal(ch, e, n);
}

static void cmd_wave(int argc, char **argv)
{
    uint32_t src, depth = 2, pre = 256, force = 0;
    if (argc < 2 || parse_u32(argv[1], &src) || (argc > 2 && parse_u32(argv[2], &depth)) ||
        (argc > 3 && parse_u32(argv[3], &pre)) || (argc > 4 && parse_u32(argv[4], &force))) {
        cprintf("ERR usage: wave <src> [depth [pretrig [force]]]\n");
        return;
    }
    if (fl_state() != FL_RUN) { cprintf("ERR fpga not ready\n"); return; }
    if (C.wave_peer.used || g_wave.pending) { cprintf("ERR busy\n"); return; }
    if (wave_request((uint8_t)src, (uint8_t)depth, (uint16_t)pre, (int)force)) { cprintf("ERR bad args\n"); return; }
    C.wave_peer = C.cur;
    C.wave_peer.used = 1;
    C.wave_count0 = g_wave.count;
}

static void cmd_slots(void)
{
    for (unsigned i = 1; i <= store_slots(); i++) {
        store_hdr_t h;
        if (store_info(i, &h) == 0)
            cprintf("%u %s nch=%u counts=%llu live_s=%.1f\n", i, h.name[0] ? h.name : "-", (unsigned)h.nch,
                    (unsigned long long)h.total,
                    (double)(h.st.total_ticks - (h.st.busy_ticks < h.st.total_ticks ? h.st.busy_ticks : h.st.total_ticks)) / MCA_FS_HZ);
        else
            cprintf("%u empty\n", i);
    }
    cprintf("OK\n");
}

static void cmd_reg(int argc, char **argv)
{
    uint32_t addr, val = 0;
    if (argc < 2 || parse_u32(argv[1], &addr) || (argc > 2 && parse_u32(argv[2], &val))) {
        cprintf("ERR usage: reg <addr> [value]\n");
        return;
    }
    peer_t *p = 0;
    for (int i = 0; i < 4; i++)
        if (!C.pend[i].used) { p = &C.pend[i]; break; }
    if (!p) { cprintf("ERR busy\n"); return; }
    *p = C.cur;
    p->used = 1;
    if (fl_cmd(argc > 2 ? MCA_CMD_WRITE_REG : MCA_CMD_READ_REG, addr, argc > 2 ? val : 0, reg_cb, p)) {
        p->used = 0;
        cprintf("ERR queue full\n");
    }
}

static void cmd_log(void)
{
    uint32_t n = C.log_pos < LOG_SIZE ? C.log_pos : LOG_SIZE;
    uint32_t start = C.log_pos - n;
    char buf[128];
    while (n) {
        uint32_t k = 0;
        while (k < sizeof(buf) && k < n) { buf[k] = C.log[(start + k) % LOG_SIZE]; k++; }
        put_raw(buf, k);
        start += k;
        n -= k;
    }
    cprintf("OK\n");
}

static void exec(int argc, char **argv)
{
    const char *c = argv[0];
    char buf[64];
    double d;
    if (!strcmp(c, "help")) cmd_help();
    else if (!strcmp(c, "status")) cmd_status();
    else if (!strcmp(c, "start")) {
        d = 0;
        if (argc > 1 && parse_f(argv[1], &d)) { cprintf("ERR bad number\n"); return; }
        app_start(d);
        cprintf("OK\n");
    } else if (!strcmp(c, "stop")) { app_stop(); cprintf("OK\n"); }
    else if (!strcmp(c, "clear")) { app_clear(); cprintf("OK\n"); }
    else if (!strcmp(c, "get")) {
        if (argc != 2 || settings_get(argv[1], buf, sizeof(buf))) cprintf("ERR unknown key\n");
        else cprintf("%s\nOK\n", buf);
    } else if (!strcmp(c, "set")) {
        if (argc != 3 || settings_set(argv[1], argv[2])) { cprintf("ERR bad key or value\n"); return; }
        app_setting_changed(argv[1]);
        if (!strcmp(argv[1], "mcu_ip") || !strcmp(argv[1], "netmask") || !strcmp(argv[1], "gateway"))
            cprintf("OK save + reboot to apply\n");
        else cprintf("OK\n");
    } else if (!strcmp(c, "dump")) {
        static char dump[1024];
        settings_dump(dump, sizeof(dump));
        put_raw(dump, strlen(dump));
        cprintf("OK\n");
    } else if (!strcmp(c, "save")) cprintf(settings_save() ? "ERR save failed\n" : "OK\n");
    else if (!strcmp(c, "defaults")) { settings_defaults(&g_set); app_setting_changed("*"); cprintf("OK\n"); }
    else if (!strcmp(c, "spec")) cmd_spec(argc, argv);
    else if (!strcmp(c, "peaks")) cmd_peaks();
    else if (!strcmp(c, "cal")) cmd_cal(argc, argv);
    else if (!strcmp(c, "calpk")) cmd_calpk(argc, argv);
    else if (!strcmp(c, "wave")) cmd_wave(argc, argv);
    else if (!strcmp(c, "store")) {
        uint32_t slot;
        if (argc < 2 || parse_u32(argv[1], &slot)) { cprintf("ERR usage: store <slot> [name]\n"); return; }
        cprintf(store_save(slot, argc > 2 ? argv[2] : 0) ? "ERR store failed\n" : "OK\n");
    } else if (!strcmp(c, "recall")) {
        uint32_t slot;
        if (argc < 2 || parse_u32(argv[1], &slot)) { cprintf("ERR usage: recall <slot>\n"); return; }
        if (store_load(slot)) { cprintf("ERR recall failed\n"); return; }
        app_stop();
        cprintf("OK\n");
    } else if (!strcmp(c, "slots")) cmd_slots();
    else if (!strcmp(c, "reg")) cmd_reg(argc, argv);
    else if (!strcmp(c, "reconfig")) {
        if (fl_state() == FL_DISCOVER) cprintf("ERR fpga not found\n");
        else { fl_reconfigure(); cprintf("OK\n"); }
    } else if (!strcmp(c, "log")) cmd_log();
    else cprintf("ERR unknown command '%s' (try help)\n", c);
}

static void run_lines(const peer_t *from, const uint8_t *data, size_t len)
{
    char line[256];
    C.cur = *from;
    C.olen = 0;
    size_t i = 0;
    while (i < len) {
        size_t n = 0;
        while (i < len && data[i] != '\n' && data[i] != '\r') {
            if (n < sizeof(line) - 1) line[n++] = (char)data[i];
            i++;
        }
        while (i < len && (data[i] == '\n' || data[i] == '\r')) i++;
        line[n] = 0;
        char *argv[MAX_TOK];
        int argc = 0;
        for (char *p = line; *p && argc < MAX_TOK;) {
            while (*p && isspace((unsigned char)*p)) p++;
            if (!*p) break;
            argv[argc++] = p;
            while (*p && !isspace((unsigned char)*p)) p++;
            if (*p) *p++ = 0;
        }
        if (argc) exec(argc, argv);
    }
    flush_out();
}

void console_rx(uint32_t ip, uint16_t port, const uint8_t *data, size_t len)
{
    peer_t p = {ip, port, 0, 0};
    run_lines(&p, data, len);
}

void console_line(const char *line, console_out_fn out)
{
    peer_t p = {0, 0, 0, out};
    run_lines(&p, (const uint8_t *)line, strlen(line));
}

void console_poll(uint32_t now)
{
    (void)now;
    if (!C.wave_peer.used) return;
    if (g_wave.count != C.wave_count0) {
        const wave_t *w = &g_wave;
        C.cur = C.wave_peer;
        C.olen = 0;
        cprintf("WAVE %u %u %u %u\n", (unsigned)w->src, (unsigned)w->total, (unsigned)w->pretrig, (unsigned)w->trig_ts);
        char line[200];
        for (unsigned i = 0; i < w->total; i += 16) {
            int k = 0;
            for (unsigned j = i; j < i + 16 && j < w->total; j++)
                k += snprintf(line + k, sizeof(line) - (size_t)k, j == i ? "%d" : " %d", w->s[j]);
            cprintf("%s\n", line);
        }
        cprintf("OK\n");
        flush_out();
        C.wave_peer.used = 0;
    } else if (!g_wave.pending) {
        reply_to(&C.wave_peer, g_wave.timed_out ? "ERR timeout (no trigger)\n" : "ERR capture failed\n");
        C.wave_peer.used = 0;
    }
}

void console_init(void)
{
    /* 日志缓冲保留 init 之前的内容 */
    memset(&C.cur, 0, sizeof(C.cur));
    memset(C.pend, 0, sizeof(C.pend));
    memset(&C.wave_peer, 0, sizeof(C.wave_peer));
    C.olen = 0;
}
