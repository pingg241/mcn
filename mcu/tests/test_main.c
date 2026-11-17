/* app/ 单元测试（宿主编译运行）：协议、寄存器换算、寻峰拟合、刻度、丢包补偿、配置、存储、FPGA 链路状态机 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "acq.h"
#include "app.h"
#include "console.h"
#include "filter_cfg.h"
#include "fpga_link.h"
#include "gfx.h"
#include "mca_proto.h"
#include "peak.h"
#include "platform.h"
#include "settings.h"
#include "spectrum.h"
#include "store.h"
#include "ui.h"
#include "wave.h"

static int fails, checks;
#define CHECK(c, ...)                                                    \
    do {                                                                 \
        checks++;                                                        \
        if (!(c)) {                                                      \
            fails++;                                                     \
            printf("  FAIL %s:%d: %s  ", __FILE__, __LINE__, #c);        \
            printf(__VA_ARGS__);                                         \
            printf("\n");                                                \
        }                                                                \
    } while (0)
#define NEAR(a, b, tol) (fabs((double)(a) - (double)(b)) <= (tol))

/* ---------------- 测试用平台 ---------------- */
static uint32_t t_ms;
static uint16_t fb[LCD_W * LCD_H];

typedef struct {
    uint16_t lport, dport;
    uint32_t dst;
    uint8_t  data[1500];
    size_t   len;
} sent_t;
static sent_t sent[256];
static int nsent;

uint32_t plat_now_ms(void) { return t_ms; }
int plat_udp_send(uint16_t lp, uint32_t dst, uint16_t dp, const void *d, size_t len)
{
    if (nsent < 256 && len <= 1500) {
        sent_t *s = &sent[nsent++];
        s->lport = lp; s->dport = dp; s->dst = dst; s->len = len;
        memcpy(s->data, d, len);
        if (len < sizeof(s->data)) s->data[len] = 0;
    }
    return 0;
}
int plat_link_speed(void) { return 100; }
uint32_t plat_local_ip(void) { return 0x7F000001u; }
uint16_t *plat_framebuffer(void) { return fb; }
void plat_lcd_flush(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
int plat_lcd_busy(void) { return 0; }
uint32_t plat_keys(void) { return 0; }
void plat_led(int on) { (void)on; }

#define NV_SLOTS 4
#define NV_SIZE  (128u * 1024u)
static uint8_t nv[NV_SLOTS][NV_SIZE];
unsigned plat_nv_slots(void) { return NV_SLOTS; }
size_t plat_nv_slot_size(void) { return NV_SIZE; }
int plat_nv_erase(unsigned s) { if (s >= NV_SLOTS) return -1; memset(nv[s], 0xFF, NV_SIZE); return 0; }
int plat_nv_write(unsigned s, size_t off, const void *d, size_t len)
{
    if (s >= NV_SLOTS || off % 32 || off + len > NV_SIZE) return -1;
    for (size_t i = 0; i < len; i++) nv[s][off + i] &= ((const uint8_t *)d)[i];   /* 闪存只能 1→0 */
    return 0;
}
int plat_nv_read(unsigned s, size_t off, void *d, size_t len)
{
    if (s >= NV_SLOTS || off + len > NV_SIZE) return -1;
    memcpy(d, nv[s] + off, len);
    return 0;
}
void plat_log(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    console_log(buf);
}

/* 确定性随机数 */
static uint32_t rng = 12345;
static double urand(void) { rng = rng * 1664525u + 1013904223u; return ((rng >> 8) + 0.5) / 16777216.0; }
static double gauss(void) { return sqrt(-2 * log(urand())) * cos(6.283185307179586 * urand()); }
static uint32_t poisson(double mu)
{
    if (mu < 30) {
        double L = exp(-mu), p = 1;
        uint32_t k = 0;
        do { k++; p *= urand(); } while (p > L);
        return k - 1;
    }
    double v = mu + sqrt(mu) * gauss();
    return v < 0 ? 0 : (uint32_t)lround(v);
}

/* ---------------- 测试 ---------------- */
static void test_proto(void)
{
    printf("proto\n");
    uint8_t b[64];
    CHECK(mca_build_mccm(b, MCA_CMD_WRITE_REG, 7, 0x10, 0x01020304) == 16, "len");
    static const uint8_t exp[16] = {'M', 'C', 'C', 'M', 2, 7, 0, 0, 0, 0, 0, 0x10, 1, 2, 3, 4};
    CHECK(memcmp(b, exp, 16) == 0, "MCCM bytes");

    uint8_t ev[16 + 16] = {'M', 'C', 'E', 'V', 1, 0, 0, 2, 0, 0, 0, 9, 0, 3, 0, 0,
                           0x12, 0x34, 0x56, 0x78, 0xAB, 0xCD, 0x09, 0, 0, 0, 0, 1, 0, 5, 0, 0};
    mca_mcev_t m;
    mca_event_t e;
    CHECK(mca_parse_mcev(ev, sizeof(ev), &m) == 0 && m.n == 2 && m.seq == 9 && m.epoch == 3, "mcev hdr");
    mca_mcev_event(&m, 0, &e);
    CHECK(e.ts == 0x312345678ull && e.energy == 0xABCD && e.flags == 9, "ev0");
    CHECK(mca_parse_mcev(ev, sizeof(ev) - 1, &m) != 0, "short mcev");

    uint8_t rs[16] = {'M', 'C', 'R', 'S', 3, 0x55, 0, 0, 0xDE, 0xAD, 0xBE, 0xEF};
    mca_mcrs_t r;
    CHECK(mca_parse_mcrs(rs, 16, &r) == 0 && r.cmd == 3 && r.seq == 0x55 && r.value == 0xDEADBEEF, "mcrs");

    /* CRC-32 标准校验值 */
    CHECK(mca_crc32(0, "123456789", 9) == 0xCBF43926u, "crc %08x", mca_crc32(0, "123456789", 9));
    CHECK(mca_crc32(mca_crc32(0, "1234", 4), "56789", 5) == 0xCBF43926u, "crc chain");
}

static void test_filter(void)
{
    printf("filter_params（对照 tools/mcaproto.py）\n");
    static const struct {
        double tau, rise, flat, fast, gap, thr;
        filter_regs_t r;
    } cases[] = {
        {32, 2, 1, 0.25, 0.25, 8, {8, 8, 64, 64, 32, 262016, 63, 42, 112, 184, 65504, 18}},
        {50, 4, 0.5, 0.1, 0.1, 3, {3, 3, 9, 128, 16, 409472, 132, 21, 155, 291, 41930, 19}},
        {1000, 10, 2, 0.5, 0.3, 20, {16, 10, 320, 320, 64, 8191872, 311, 82, 408, 736, 53686, 25}},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        filter_regs_t r;
        filter_params(cases[i].tau, cases[i].rise, cases[i].flat, cases[i].fast, cases[i].gap, cases[i].thr, 65280, &r);
        CHECK(memcmp(&r, &cases[i].r, sizeof(r)) == 0, "case %d: k=%u m=%u pz=%u gain=%u shift=%u", (int)i,
              (unsigned)r.slow_k, (unsigned)r.slow_m, (unsigned)r.pz_m, (unsigned)r.energy_gain,
              (unsigned)r.energy_shift);
    }
}

static void synth(uint32_t *h, int n, const double *c, const double *s, const double *a, int np, double b0, double b1)
{
    for (int i = 0; i < n; i++) {
        double mu = b0 + b1 * i;
        for (int k = 0; k < np; k++) {
            double z0 = (i - 0.5 - c[k]) / s[k], z1 = (i + 0.5 - c[k]) / s[k];
            mu += a[k] * 0.5 * (erf(z1 / sqrt(2.0)) - erf(z0 / sqrt(2.0)));
        }
        h[i] = poisson(mu < 0 ? 0 : mu);
    }
}

static void test_peaks(void)
{
    printf("peak_search / peak_fit\n");
    static uint32_t h[4096];
    peak_t pk[8];

    double c1[] = {1000.3}, s1[] = {12}, a1[] = {1e5};
    synth(h, 4096, c1, s1, a1, 1, 100, -0.01);
    int n = peak_search(h, 4096, pk, 8);
    CHECK(n >= 1, "n=%d", n);
    if (n >= 1) {
        CHECK(NEAR(pk[0].centroid, 1000.3, 0.2), "centroid %.3f", pk[0].centroid);
        CHECK(NEAR(pk[0].sigma, 12, 0.36), "sigma %.3f", pk[0].sigma);
        CHECK(NEAR(pk[0].fwhm, 12 * 2.3548, 0.8), "fwhm %.3f", pk[0].fwhm);
        CHECK(NEAR(pk[0].area, 1e5, 3e3), "area %.0f", pk[0].area);
        CHECK(pk[0].area_err > 250 && pk[0].area_err < 800, "area_err %.1f", pk[0].area_err);
        CHECK(pk[0].chi2ndf > 0.5 && pk[0].chi2ndf < 2, "chi2ndf %.2f", pk[0].chi2ndf);
    }

    /* 两个峰（类 60Co）+ 指数连续本底 */
    double c2[] = {2346, 2664}, s2[] = {15, 16}, a2[] = {5e4, 4.5e4};
    synth(h, 4096, c2, s2, a2, 2, 0, 0);
    for (int i = 0; i < 4096; i++) h[i] += poisson(400 * exp(-i / 900.0) + (i < 2100 ? 30 : 5));
    n = peak_search(h, 4096, pk, 8);
    CHECK(n >= 2, "n=%d", n);
    if (n >= 2) {
        CHECK(NEAR(pk[0].centroid, 2346, 0.5) && NEAR(pk[1].centroid, 2664, 0.5), "c %.2f %.2f", pk[0].centroid,
              pk[1].centroid);
        CHECK(NEAR(pk[0].area, 5e4, 2.5e3) && NEAR(pk[1].area, 4.5e4, 2.3e3), "a %.0f %.0f", pk[0].area, pk[1].area);
    }
    /* NaI 宽度的 Co-60 双峰（σ≈50 道，比 SNIP 默认窗口宽）：初值不准时也要拟合到位 */
    double c3[] = {2200, 2498}, s3[] = {49, 52}, a3[] = {2e4, 2e4};
    synth(h, 4096, c3, s3, a3, 2, 0, 0);
    for (int i = 0; i < 4096; i++) h[i] += poisson(30 * exp(-i / 1500.0) + 15 / (1 + exp((i - 2000) / 40.0)));
    n = peak_search(h, 4096, pk, 8);
    CHECK(n == 2, "n=%d", n);
    if (n == 2) {
        double ca = pk[0].centroid < pk[1].centroid ? pk[0].centroid : pk[1].centroid;
        double cb = pk[0].centroid < pk[1].centroid ? pk[1].centroid : pk[0].centroid;
        CHECK(NEAR(ca, 2200, 1.5) && NEAR(cb, 2498, 1.5), "NaI c %.2f %.2f", ca, cb);
        CHECK(NEAR(pk[0].area, 2e4, 1e3) && NEAR(pk[1].area, 2e4, 1e3), "NaI a %.0f %.0f", pk[0].area, pk[1].area);
    }
    /* 纯本底不应报峰 */
    for (int i = 0; i < 4096; i++) h[i] = poisson(200 * exp(-i / 1500.0) + 3);
    n = peak_search(h, 4096, pk, 8);
    CHECK(n == 0, "false peaks %d (first at %.1f)", n, n ? pk[0].centroid : 0.0);
}

static void test_cal(void)
{
    printf("spec_calibrate\n");
    float c[3];
    double ch2[] = {100, 300}, e2[] = {50, 150};
    CHECK(spec_calibrate(ch2, e2, 2, c) == 0 && NEAR(c[0], 0, 1e-4) && NEAR(c[1], 0.5, 1e-6) && c[2] == 0, "2pt");
    double ch1[] = {200}, e1[] = {661.657};
    CHECK(spec_calibrate(ch1, e1, 1, c) == 0 && NEAR(c[1], 661.657 / 200, 1e-6) && c[0] == 0, "1pt");
    double ch3[] = {100, 500, 1000, 2000}, e3[4];
    for (int i = 0; i < 4; i++) e3[i] = 5 + 0.3 * ch3[i] + 2e-5 * ch3[i] * ch3[i];
    CHECK(spec_calibrate(ch3, e3, 4, c) == 0 && NEAR(c[0], 5, 1e-3) && NEAR(c[1], 0.3, 1e-6) && NEAR(c[2], 2e-5, 1e-9),
          "quad %g %g %g", c[0], c[1], c[2]);
    memcpy(g_set.cal, c, sizeof(c));
    CHECK(NEAR(spec_e2ch(spec_ch2e(1234.5)), 1234.5, 1e-6), "e2ch");
    settings_defaults(&g_set);
}

static size_t mk_mcev(uint8_t *b, uint32_t seq, int n, uint16_t energy, uint8_t flags)
{
    memset(b, 0, 16 + 8 * n);
    memcpy(b, "MCEV", 4);
    b[4] = 1;
    mca_put_be16(b + 6, (uint16_t)n);
    mca_put_be32(b + 8, seq);
    for (int i = 0; i < n; i++) {
        mca_put_be32(b + 16 + 8 * i, seq * 1000u + (uint32_t)i);
        mca_put_be16(b + 20 + 8 * i, energy);
        b[22 + 8 * i] = flags;
    }
    return 16u + 8u * (unsigned)n;
}

static void test_acq(void)
{
    printf("acq 丢包补偿 / 活时间\n");
    uint8_t b[16 + 8 * 64];
    mca_mcev_t m;
    settings_defaults(&g_set);
    spec_reset(4096, 4);
    acq_clear();
    for (uint32_t seq = 100; seq < 200; seq++) {
        if (seq % 10 == 3) continue;                   /* 丢 10 个包 */
        size_t len = mk_mcev(b, seq, 10, (uint16_t)(seq == 150 ? 0x1000 : 0x2000), seq == 151 ? MCA_EV_PILEUP : 0);
        mca_parse_mcev(b, len, &m);
        acq_on_mcev(&m, 0);
    }
    mca_parse_mcev(b, mk_mcev(b, 150, 10, 0x2000, 0), &m);
    acq_on_mcev(&m, 0);                                /* 重复包 */
    acq_stats_t s;
    acq_stats(&s);
    CHECK(s.lost_pkts == 10 && s.loss_factor == 1.0, "无心跳时只看序号空洞：lost %llu", (unsigned long long)s.lost_pkts);

    /* FPGA 实际发了 100..204 共 105 包（末尾 200..204 全丢），每包 10 事件，另有 18 个 FIFO 满丢弃 */
    mca_mcst_t st = {0};
    st.pkt_seq = 205;
    st.total_ticks = 32000000ull * 10;
    st.busy_ticks  = 32000000ull * 2;
    st.total_events = 1068;
    st.pileup_events = 10;
    st.dropped_events = 18;
    acq_on_mcst(&st);
    acq_stats(&s);
    CHECK(g_acq.pkts == 90 && g_acq.events == 900 && g_acq.dup_pkts == 1, "pkts %llu ev %llu dup %llu",
          (unsigned long long)g_acq.pkts, (unsigned long long)g_acq.events, (unsigned long long)g_acq.dup_pkts);
    CHECK(s.lost_pkts == 15 && NEAR(s.lost_pkt_pct, 100.0 * 15 / 105, 1e-9), "末尾丢包靠 pkt_seq 发现：lost %llu",
          (unsigned long long)s.lost_pkts);
    CHECK(s.counts == 890 && g_spec.h[0x2000 >> 4] == 880 && g_spec.h[0x1000 >> 4] == 10, "counts %llu",
          (unsigned long long)s.counts);
    CHECK(s.lost_events == 168 && NEAR(s.loss_factor, 1068 / 900.0, 1e-12), "按 total_events 补偿：lost %llu loss %.5f",
          (unsigned long long)s.lost_events, s.loss_factor);
    CHECK(NEAR(s.real_s, 10, 1e-9) && NEAR(s.live_s, 8, 1e-9) && NEAR(s.dead_pct, 20, 1e-9), "times");
    CHECK(NEAR(s.icr, 106.8, 1e-9) && NEAR(s.rate, 890 * s.loss_factor / 8, 1e-9), "rates");

    /* 心跳之后到的包不改变补偿系数（两边都取心跳时刻） */
    mca_parse_mcev(b, mk_mcev(b, 205, 10, 0x2000, 0), &m);
    acq_on_mcev(&m, 0);
    acq_stats(&s);
    CHECK(s.lost_pkts == 15 && NEAR(s.loss_factor, 1068 / 900.0, 1e-12), "after hb: lost %llu loss %.5f",
          (unsigned long long)s.lost_pkts, s.loss_factor);

    /* PU_DISCARD：被 FPGA 丢掉的堆积事件不算丢失 */
    st.total_events = 1068 + 50;
    st.pileup_events = 10 + 50;
    st.status_flags = MCA_SF_PU_DISCARD;
    acq_on_mcst(&st);
    acq_stats(&s);
    CHECK(NEAR(s.loss_factor, (1118 - 60) / 910.0, 1e-12), "pu_discard loss %.5f", s.loss_factor);
}

static void test_settings_store(void)
{
    printf("settings / store\n");
    char buf[1024];
    settings_defaults(&g_set);
    CHECK(settings_set("tau_us", "45.5") == 0 && g_set.tau_us == 45.5f, "tau");
    CHECK(settings_set("nch", "3000") != 0 && settings_set("nch", "2048") == 0 && g_set.nch == 2048, "nch");
    CHECK(settings_set("fpga_ip", "10.0.0.7") == 0 && g_set.fpga_ip == 0x0A000007u, "ip");
    CHECK(settings_set("rise_us", "1000") != 0, "range");
    CHECK(settings_get("fpga_ip", buf, sizeof(buf)) == 0 && strcmp(buf, "10.0.0.7") == 0, "get ip '%s'", buf);
    settings_dump(buf, sizeof(buf));
    CHECK(strstr(buf, "tau_us=45.5\n") != 0 && strstr(buf, "cal_unit=ch") != 0, "dump");
    CHECK(settings_save() == 0, "save");
    settings_t keep = g_set;
    settings_defaults(&g_set);
    CHECK(settings_load() == 0 && memcmp(&keep, &g_set, sizeof(keep)) == 0, "load");
    nv[0][20] ^= 1;
    CHECK(settings_load() != 0 && g_set.tau_us == 32.0f, "crc reject");

    settings_defaults(&g_set);
    spec_reset(4096, 4);
    acq_clear();
    for (int i = 0; i < 4096; i++) g_spec.h[i] = (uint32_t)(i * 7 % 1000);
    g_spec.total = 123456;
    g_acq.have_st = 1;
    g_acq.st.total_ticks = 640000000ull;
    g_acq.st.total_events = 125000;
    g_acq.counts_at_st = 120000;
    g_acq.events_at_st = 121000;
    acq_stats_t s0, s1;
    acq_stats(&s0);
    CHECK(store_slots() == NV_SLOTS - 1, "slots");
    CHECK(store_save(2, "co60") == 0, "store");
    spec_reset(4096, 4);
    CHECK(store_load(2) == 0 && g_spec.h[4095] == 4095u * 7 % 1000 && g_spec.total == 123456 && g_acq.frozen &&
          g_acq.st.total_ticks == 640000000ull, "recall");
    acq_stats(&s1);
    CHECK(s1.loss_factor == s0.loss_factor && s1.rate == s0.rate && s1.loss_factor > 1.03,
          "调出后补偿系数与计数率不变：%.5f %.1f", s1.loss_factor, s1.rate);
    store_hdr_t h;
    CHECK(store_info(2, &h) == 0 && strcmp(h.name, "co60") == 0 && store_info(1, &h) != 0, "info");
    nv[2][STORE_HDR_SIZE + 100] ^= 0x10;
    CHECK(store_load(2) != 0, "crc reject");
    CHECK(store_save(9, 0) != 0, "bad slot");
}

/* ---------------- FPGA 链路状态机 ---------------- */
static int find_sent(uint8_t cmd, int from)
{
    for (int i = from; i < nsent; i++)
        if (sent[i].len == 16 && !memcmp(sent[i].data, "MCCM", 4) && sent[i].data[4] == cmd) return i;
    return -1;
}

static char local_buf[4096];
static size_t local_len;
static void local_out(const char *s, size_t n)
{
    if (local_len + n >= sizeof(local_buf)) return;
    memcpy(local_buf + local_len, s, n);
    local_len += n;
    local_buf[local_len] = 0;
}

static void answer_last(uint8_t status, uint32_t value)
{
    const sent_t *s = &sent[nsent - 1];
    uint8_t r[16] = {'M', 'C', 'R', 'S', s->data[4], s->data[5], status, 0};
    mca_put_be32(r + 8, value);
    app_udp_rx(MCA_PORT_CMD, g_set.fpga_ip, MCA_PORT_CMD, r, sizeof(r));
}

static void test_link(void)
{
    printf("fpga_link 发现 → 配置 → 运行\n");
    memset(nv, 0xFF, sizeof(nv));
    t_ms = 1000;
    nsent = 0;
    app_init();
    CHECK(fl_state() == FL_DISCOVER, "start");
    app_poll();
    CHECK(nsent == 1 && sent[0].data[4] == MCA_CMD_PING && sent[0].dst == g_set.fpga_ip && sent[0].dport == 5000,
          "ping sent (nsent %d)", nsent);
    /* 不应答：100 ms 后重发 */
    t_ms += 101;
    app_poll();
    CHECK(nsent == 2 && sent[1].data[4] == MCA_CMD_PING && sent[1].data[5] == sent[0].data[5] + 1, "retry");
    answer_last(MCA_ST_OK, 0x00010000);
    CHECK(fl_state() == FL_CONFIG && fl_version() == 0x00010000, "config");

    /* 逐条应答配置命令 */
    reg_write_t w[FILTER_CFG_MAX];
    int nw = filter_cfg_build(w, FILTER_CFG_MAX);
    int first = nsent, guard = 0;
    while (fl_state() == FL_CONFIG && guard++ < 64) {
        int before = nsent;
        app_poll();
        if (nsent == before) break;
        answer_last(MCA_ST_OK, 0);
    }
    CHECK(fl_state() == FL_RUN, "run (state %s)", fl_state_name());
    int ncmd = nsent - first;
    CHECK(ncmd == nw + 4, "cmd count %d vs %d", ncmd, nw + 4);
    CHECK(sent[first].data[4] == MCA_CMD_WRITE_REG && mca_be32(sent[first].data + 8) == MCA_REG_CTRL &&
          mca_be32(sent[first].data + 12) == 0, "CTRL=0 first");
    for (int i = 0; i < nw && first + 1 + i < nsent; i++)
        CHECK(mca_be32(sent[first + 1 + i].data + 8) == w[i].addr && mca_be32(sent[first + 1 + i].data + 12) == w[i].value,
              "reg %d", i);
    int ss = find_sent(MCA_CMD_SET_STREAM, first);
    CHECK(ss > 0 && mca_be32(sent[ss].data + 8) == g_set.stream_n && mca_be32(sent[ss].data + 12) == g_set.stream_ms,
          "set_stream");
    CHECK(mca_be32(sent[nsent - 1].data + 12) == filter_cfg_ctrl(1) && (filter_cfg_ctrl(1) & MCA_CTRL_RUN), "CTRL run");

    /* 事件进谱、心跳 */
    uint8_t b[16 + 8 * 64];
    size_t len = mk_mcev(b, 1, 64, 0x3000, 0);
    app_udp_rx(MCA_PORT_EV, g_set.fpga_ip, MCA_PORT_EV, b, len);
    app_udp_rx(MCA_PORT_EV, g_set.fpga_ip + 1, MCA_PORT_EV, b, len);     /* 别的地址：忽略 */
    CHECK(g_spec.total == 64 && g_spec.h[0x3000 >> settings_shift()] == 64, "events");
    uint8_t hb[64] = {'M', 'C', 'S', 'T', 1};
    mca_put_be32(hb + 8, 5);
    mca_put_be32(hb + 36, 32000000);
    mca_put_be32(hb + 52, MCA_SF_RUN | MCA_SF_LINK);
    app_udp_rx(MCA_PORT_CMD, g_set.fpga_ip, MCA_PORT_CMD, hb, sizeof(hb));
    CHECK(fl_running(), "running");

    /* 控制台：status 与 spec */
    nsent = 0;
    const char *cmd = "status\nspec 0 20\n";
    console_rx(0x7F000001u, 40000, (const uint8_t *)cmd, strlen(cmd));
    CHECK(nsent >= 1 && sent[0].dport == 40000 && sent[0].lport == MCA_PORT_CONSOLE, "console reply");
    CHECK(strstr((char *)sent[0].data, "state RUN\n") != 0, "status text");
    CHECK(strstr((char *)sent[nsent - 1].data, "SPEC 0 20 4096 64\n") != 0, "spec text");

    /* 本机终端（宿主版命令行窗口）：回复交给回调不走 UDP；reg 要等 FPGA 应答，结果也回到这里 */
    nsent = 0;
    local_len = 0;
    console_line("status", local_out);
    CHECK(nsent == 0 && strstr(local_buf, "state RUN\n") && strstr(local_buf, "\nOK\n"), "local console status");
    local_len = 0;
    local_buf[0] = 0;
    console_line("reg 0", local_out);
    app_poll();
    CHECK(nsent >= 1 && sent[nsent - 1].dport == MCA_PORT_CMD, "local reg → FPGA");
    if (nsent >= 1) answer_last(MCA_ST_OK, 0x4D434131u);
    int udp_console = 0;
    for (int i = 0; i < nsent; i++) udp_console += sent[i].lport == MCA_PORT_CONSOLE;
    CHECK(!udp_console && strstr(local_buf, "0x4D434131 1296253233\nOK\n"), "local reg reply: %s", local_buf);

    /* 修改滤波参数：重新配置，且清谱 */
    nsent = 0;
    cmd = "set rise_us 4\n";
    console_rx(0x7F000001u, 40000, (const uint8_t *)cmd, strlen(cmd));
    CHECK(fl_state() == FL_CONFIG, "reconfig on set");
    guard = 0;
    while (fl_state() == FL_CONFIG && guard++ < 64) {
        int before = nsent;
        app_poll();
        if (nsent == before) break;
        answer_last(MCA_ST_OK, 0);
    }
    CHECK(fl_state() == FL_RUN && g_spec.total == 0, "cleared after reconfig (total %llu)",
          (unsigned long long)g_spec.total);

    /* 心跳超时 → 重新发现 */
    t_ms += 4000;
    app_poll();
    CHECK(fl_state() == FL_DISCOVER, "hb timeout");

    /* 画一遍界面，确保不越界 */
    for (int s = 0; s < UI_NSCREENS; s++) { ui_set_screen(s); ui_draw(); }
    CHECK(fb[0] != 0 || fb[LCD_W * LCD_H - 1] != 0, "drawn");
}

static void test_wave(void)
{
    printf("wave 分包重组\n");
    memset(&g_wave, 0, sizeof(g_wave));
    uint8_t b[24 + 512];
    for (int part = 0; part < 4; part++) {
        memset(b, 0, sizeof(b));
        memcpy(b, "MCWF", 4);
        b[4] = 1;
        b[5] = 3;
        mca_put_be16(b + 6, 256);
        mca_put_be32(b + 8, 77);
        mca_put_be16(b + 12, (uint16_t)(part * 256));
        mca_put_be16(b + 14, 1024);
        mca_put_be32(b + 16, 999);
        mca_put_be16(b + 20, 100);
        for (int i = 0; i < 256; i++) mca_put_be16(b + 24 + 2 * i, (uint16_t)(int16_t)(part * 256 + i - 500));
        mca_mcwf_t w;
        CHECK(mca_parse_mcwf(b, sizeof(b), &w) == 0, "parse");
        wave_on_mcwf(&w);
        CHECK(g_wave.count == (part == 3 ? 1u : 0u), "count after part %d", part);
    }
    CHECK(g_wave.total == 1024 && g_wave.pretrig == 100 && g_wave.src == 3 && g_wave.s[0] == -500 &&
          g_wave.s[1023] == 523, "content");
}

int main(void)
{
    test_proto();
    test_filter();
    test_peaks();
    test_cal();
    test_acq();
    test_settings_store();
    test_wave();
    test_link();
    printf("%d/%d 通过%s\n", checks - fails, checks, fails ? "，有失败" : "");
    return fails ? 1 : 0;
}
