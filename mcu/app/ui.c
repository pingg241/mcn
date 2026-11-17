#include "ui.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "acq.h"
#include "app.h"
#include "fpga_link.h"
#include "gfx.h"
#include "platform.h"
#include "settings.h"
#include "spectrum.h"
#include "wave.h"

#define REDRAW_MS   250
#define DEBOUNCE_MS 30
#define LONG_MS     1000
#define WAVE_MS     1000      /* 波形页刷新间隔 */

/* 绘图区 */
#define PX0 46
#define PX1 474
#define PY0 26
#define PY1 262
#define PW  (PX1 - PX0)
#define PH  (PY1 - PY0)

static struct {
    int screen;
    uint32_t t_draw;
    uint32_t key_raw, key_stable, t_key, t_press;
    int long_fired;
    uint8_t wave_src;
    uint32_t t_wave;
    uint32_t wave_seen;
} U;

/* ---------------- 格式化 ---------------- */
static void fmt_si(char *out, size_t n, double v)
{
    double a = fabs(v);
    if (a < 1e3)       snprintf(out, n, a < 10 && a != floor(a) ? "%.2f" : "%.0f", v);
    else if (a < 1e4)  snprintf(out, n, "%.2fk", v / 1e3);
    else if (a < 1e6)  snprintf(out, n, "%.1fk", v / 1e3);
    else if (a < 1e7)  snprintf(out, n, "%.2fM", v / 1e6);
    else if (a < 1e9)  snprintf(out, n, "%.1fM", v / 1e6);
    else               snprintf(out, n, "%.2fG", v / 1e9);
}

static void fmt_time(char *out, size_t n, double s)
{
    if (s < 1000) snprintf(out, n, "%.1f", s);
    else if (s < 1e5) snprintf(out, n, "%.0f", s);
    else snprintf(out, n, "%.0fh", s / 3600);
}

static void fmt_ip(char *out, size_t n, uint32_t ip)
{
    snprintf(out, n, "%u.%u.%u.%u", (unsigned)(ip >> 24), (unsigned)(ip >> 16) & 255, (unsigned)(ip >> 8) & 255,
             (unsigned)ip & 255);
}

static double nice_step(double range, int nticks)
{
    if (range <= 0) return 1;
    double raw = range / nticks, mag = pow(10, floor(log10(raw))), f = raw / mag;
    return (f < 1.5 ? 1 : f < 3 ? 2 : f < 7 ? 5 : 10) * mag;
}

static void fmt_tick(char *out, size_t n, double v, double step)
{
    if (step >= 1 || v == 0) fmt_si(out, n, v);
    else if (step >= 0.1) snprintf(out, n, "%.1f", v);
    else snprintf(out, n, "%.2f", v);
}

static int uncalibrated(void)
{
    return strcmp(g_set.cal_unit, "ch") == 0 || (g_set.cal[0] == 0 && g_set.cal[1] == 1 && g_set.cal[2] == 0);
}

/* ---------------- 公共部件 ---------------- */
static void status_bar(void)
{
    acq_stats_t s;
    char a[24], b[24];
    acq_stats(&s);
    gfx_fill(0, 0, LCD_W, 22, C_PANEL);

    const char *tag;
    uint16_t c;
    if (g_acq.frozen)                    { tag = "RECALL"; c = C_SPEC; }
    else if (fl_state() == FL_DISCOVER)  { tag = "NO FPGA"; c = C_BAD; }
    else if (fl_state() == FL_CONFIG)    { tag = "CONFIG"; c = C_WARN; }
    else if (fl_running())               { tag = "RUN"; c = C_GOOD; }
    else                                 { tag = "STOP"; c = C_WARN; }
    gfx_text(4, 3, tag, c, GFX_TRANSPARENT, 0);

    fmt_time(a, sizeof(a), s.real_s);
    gfx_printf(64, 3, C_TEXT, GFX_TRANSPARENT, 0, "R %s", a);
    fmt_time(a, sizeof(a), s.live_s);
    gfx_printf(136, 3, C_TEXT, GFX_TRANSPARENT, 0, "L %s", a);
    gfx_printf(208, 3, s.dead_pct > 50 ? C_BAD : s.dead_pct > 20 ? C_WARN : C_TEXT, GFX_TRANSPARENT, 0,
               "DT %.1f%%", s.dead_pct);
    fmt_si(a, sizeof(a), s.icr);
    gfx_printf(280, 3, C_TEXT, GFX_TRANSPARENT, 0, "ICR %s", a);
    fmt_si(b, sizeof(b), (double)s.counts);
    gfx_printf(368, 3, C_TEXT, GFX_TRANSPARENT, 0, "N %s", b);

    int sp = plat_link_speed();
    snprintf(a, sizeof(a), sp ? "%dM" : "--", sp);
    gfx_text(LCD_W - 4 - gfx_text_width(a, 0), 3, a, sp ? C_DIM : C_BAD, GFX_TRANSPARENT, 0);
}

static void plot_frame(void)
{
    gfx_fill(0, 22, LCD_W, LCD_H - 22, C_BG);
    gfx_rect(PX0 - 1, PY0 - 1, PW + 2, PH + 2, C_AXIS);
}

/* ---------------- 能谱页 ---------------- */
static int ch2px(double ch) { return PX0 + (int)lround(ch * PW / g_spec.nch); }

static void draw_spectrum(void)
{
    static uint32_t col[PW];
    static int ytop[PW];
    const uint32_t nch = g_spec.nch ? g_spec.nch : 1;
    uint32_t vmax = 0;
    char buf[64];

    plot_frame();
    for (int i = 0; i < PW; i++) {
        uint32_t lo = (uint32_t)((uint64_t)i * nch / PW), hi = (uint32_t)((uint64_t)(i + 1) * nch / PW);
        if (hi <= lo) hi = lo + 1;
        uint32_t m = 0;
        for (uint32_t c = lo; c < hi && c < nch; c++)
            if (g_spec.h[c] > m) m = g_spec.h[c];
        col[i] = m;
        if (m > vmax) vmax = m;
    }

    /* 纵轴 */
    const int logy = g_set.log_scale;
    double top, step = 1;
    int decades = 1;
    if (logy) {
        decades = vmax > 1 ? (int)ceil(log10((double)vmax) + 1e-9) : 1;
        if (decades < 1) decades = 1;
        top = decades;
        for (int d = 0; d <= decades; d++) {
            int y = PY1 - 1 - (int)lround((double)d / decades * (PH - 1));
            if (d > 0 && d < decades) gfx_hline(PX0, y, PW, C_GRID);
            fmt_si(buf, sizeof(buf), pow(10, d));
            gfx_text(PX0 - 4 - gfx_text_width(buf, 0), y - 8 < PY0 - 4 ? PY0 - 4 : y - 8, buf, C_DIM,
                     GFX_TRANSPARENT, 0);
        }
    } else {
        double vm = vmax < 10 ? 10 : vmax * 1.05;
        step = nice_step(vm, 5);
        top = ceil(vm / step) * step;
        for (double v = 0; v <= top + step / 2; v += step) {
            int y = PY1 - 1 - (int)lround(v / top * (PH - 1));
            if (v > 0 && v < top - step / 2) gfx_hline(PX0, y, PW, C_GRID);
            fmt_si(buf, sizeof(buf), v);
            gfx_text(PX0 - 4 - gfx_text_width(buf, 0), y - 8 < PY0 - 4 ? PY0 - 4 : y - 8, buf, C_DIM,
                     GFX_TRANSPARENT, 0);
        }
    }

    /* 横轴：未刻度用道址，刻度后用能量 */
    const int cal = !uncalibrated();
    double x0 = cal ? spec_ch2e(0) : 0, x1 = cal ? spec_ch2e(nch) : nch;
    if (x1 < x0) { double t = x0; x0 = x1; x1 = t; }
    double xs = nice_step(x1 - x0, 6);
    int last_label = -100;
    for (double v = ceil(x0 / xs) * xs; v <= x1 + 1e-9; v += xs) {
        int px = ch2px(cal ? spec_e2ch(v) : v);
        if (px < PX0 || px >= PX1) continue;
        gfx_vline(px, PY0, PH, C_GRID);
        gfx_vline(px, PY1, 4, C_AXIS);
        fmt_tick(buf, sizeof(buf), v, xs);
        int w = gfx_text_width(buf, 0), lx = px - w / 2;
        if (lx < 0) lx = 0;
        if (lx + w > LCD_W) lx = LCD_W - w;
        if (lx > last_label + 4) {
            gfx_text(lx, PY1 + 3, buf, C_DIM, GFX_TRANSPARENT, 0);
            last_label = lx + w;
        }
    }
    snprintf(buf, sizeof(buf), "%s", cal ? g_set.cal_unit : "ch");
    gfx_text(4, PY1 + 3, buf, C_DIM, GFX_TRANSPARENT, 0);

    /* 谱线：填充 + 轮廓 */
    int prev = -1;
    for (int i = 0; i < PW; i++) {
        double f;
        if (!col[i]) f = -1;
        else if (logy) f = log10((double)col[i]) / top;
        else f = col[i] / top;
        int y = f < 0 ? PY1 : PY1 - 1 - (int)lround((f < 0 ? 0 : f > 1 ? 1 : f) * (PH - 1));
        if (col[i] && y > PY1 - 2) y = PY1 - 2;       /* 非零计数至少画一点 */
        ytop[i] = y;
        if (y < PY1) gfx_vline(PX0 + i, y, PY1 - y, C_FILL);
        if (prev >= 0) {
            int a = prev < y ? prev : y, b = prev < y ? y : prev;
            if (a > PY1 - 1) a = PY1 - 1;
            if (b > PY1 - 1) b = PY1 - 1;
            gfx_vline(PX0 + i, a, b - a + 1, C_SPEC);
        }
        prev = y < PY1 ? y : PY1 - 1;
    }

    /* 峰标记 */
    for (int k = 0; k < g_spec.npeaks; k++) {
        const peak_t *p = &g_spec.peaks[k];
        int px = ch2px(p->centroid);
        if (px < PX0 || px >= PX1) continue;
        int yt = ytop[px - PX0] - 4;
        for (int y = PY0 + 18; y < yt; y += 3) gfx_pixel(px, y, C_PEAK);
        snprintf(buf, sizeof(buf), "%d", k + 1);
        gfx_text(px - 4, PY0 + 1, buf, C_PEAK, GFX_TRANSPARENT, 0);
    }

    /* 底部：面积最大的两个峰 */
    for (int k = 0; k < 2; k++) {
        int y = 284 + 18 * k;
        if (k < g_spec.npeaks) {
            const peak_t *p = &g_spec.peaks[k];
            char area[16];
            fmt_si(area, sizeof(area), p->area);
            if (cal)
                gfx_printf(4, y, C_TEXT, GFX_TRANSPARENT, 0, "%d: %.2f %s  FWHM %.2f (%.2f%%)  N %s+-%.0f", k + 1,
                           p->energy, g_set.cal_unit, p->fwhm_e, p->res_pct, area, p->area_err);
            else
                gfx_printf(4, y, C_TEXT, GFX_TRANSPARENT, 0, "%d: ch %.2f  FWHM %.2f (%.2f%%)  N %s+-%.0f", k + 1,
                           p->centroid, p->fwhm, p->centroid > 0 ? 100 * p->fwhm / p->centroid : 0.0, area,
                           p->area_err);
        } else if (k == 0) {
            gfx_printf(4, y, C_DIM, GFX_TRANSPARENT, 0, "No peaks  (%u ch, %s)", (unsigned)g_spec.nch,
                       logy ? "log" : "lin");
        } else {
            gfx_text(4, y, "KEY: next page   hold: clear", C_DIM, GFX_TRANSPARENT, 0);
        }
    }
}

/* ---------------- 波形页 ---------------- */
static void wave_service(uint32_t now)
{
    if (U.screen != UI_WAVE || fl_state() != FL_RUN || g_wave.pending) return;
    if (now - U.t_wave < WAVE_MS) return;
    U.t_wave = now;
    /* 上次等不到触发就强制捕获一段，至少能看到基线 */
    wave_request(U.wave_src, 2, 256, g_wave.timed_out);
}

static void draw_wave(void)
{
    char buf[64];
    plot_frame();
    const wave_t *w = &g_wave;
    if (!w->count) {
        gfx_printf(PX0 + 8, PY0 + 8, C_DIM, GFX_TRANSPARENT, 0, "%s waiting for waveform...",
                   wave_src_name(U.wave_src));
    } else {
        int vmin = w->s[0], vmax = w->s[0];
        for (unsigned i = 1; i < w->total; i++) {
            if (w->s[i] < vmin) vmin = w->s[i];
            if (w->s[i] > vmax) vmax = w->s[i];
        }
        double span = vmax - vmin < 16 ? 16 : vmax - vmin;
        double lo = (vmax + vmin) / 2.0 - span * 0.55, hi = (vmax + vmin) / 2.0 + span * 0.55;
        double ys = nice_step(hi - lo, 5);
        for (double v = ceil(lo / ys) * ys; v <= hi; v += ys) {
            int y = PY1 - 1 - (int)lround((v - lo) / (hi - lo) * (PH - 1));
            gfx_hline(PX0, y, PW, C_GRID);
            snprintf(buf, sizeof(buf), "%.0f", v);
            gfx_text(PX0 - 4 - gfx_text_width(buf, 0), y - 8, buf, C_DIM, GFX_TRANSPARENT, 0);
        }
        /* 横轴 µs，0 点在触发处 */
        double t0 = -(double)w->pretrig / 32.0, t1 = (double)(w->total - w->pretrig) / 32.0;
        double xs = nice_step(t1 - t0, 6);
        for (double t = ceil(t0 / xs) * xs; t <= t1 + 1e-9; t += xs) {
            int px = PX0 + (int)lround((t - t0) / (t1 - t0) * (PW - 1));
            gfx_vline(px, PY0, PH, C_GRID);
            fmt_tick(buf, sizeof(buf), t, xs);
            gfx_text(px - gfx_text_width(buf, 0) / 2, PY1 + 3, buf, C_DIM, GFX_TRANSPARENT, 0);
        }
        gfx_text(4, PY1 + 3, "us", C_DIM, GFX_TRANSPARENT, 0);
        int ptx = PX0 + (int)lround((double)w->pretrig / w->total * (PW - 1));
        for (int y = PY0; y < PY1; y += 4) gfx_vline(ptx, y, 2, C_PEAK);

        /* 每列取 min/max，保证 4096 点压到 428 列时脉冲尖峰不丢 */
        int prev_hi = -1, prev_lo = -1;
        for (int i = 0; i < PW; i++) {
            unsigned a = (unsigned)((uint64_t)i * w->total / PW), b = (unsigned)((uint64_t)(i + 1) * w->total / PW);
            if (b <= a) b = a + 1;
            int mn = 32767, mx = -32768;
            for (unsigned j = a; j < b && j < w->total; j++) {
                if (w->s[j] < mn) mn = w->s[j];
                if (w->s[j] > mx) mx = w->s[j];
            }
            int yh = PY1 - 1 - (int)lround((mx - lo) / (hi - lo) * (PH - 1));
            int yl = PY1 - 1 - (int)lround((mn - lo) / (hi - lo) * (PH - 1));
            if (prev_hi >= 0) {           /* 与前一列连起来 */
                if (yh > prev_lo) yh = prev_lo;
                if (yl < prev_hi) yl = prev_hi;
            }
            int t = yh < yl ? yh : yl, bt = yh < yl ? yl : yh;
            gfx_vline(PX0 + i, t, bt - t + 1, C_WAVE);
            prev_hi = PY1 - 1 - (int)lround((mx - lo) / (hi - lo) * (PH - 1));
            prev_lo = PY1 - 1 - (int)lround((mn - lo) / (hi - lo) * (PH - 1));
        }
        gfx_printf(PX0 + 6, PY0 + 4, C_TEXT, GFX_TRANSPARENT, 0, "%s  %u pts  #%u", wave_src_name(w->src),
                   (unsigned)w->total, (unsigned)w->count);
    }
    const char *st = fl_state() != FL_RUN ? "FPGA not ready" : g_wave.pending ? "waiting trigger"
                     : g_wave.timed_out ? "no trigger, forced" : "ok";
    gfx_printf(4, 284, C_TEXT, GFX_TRANSPARENT, 0, "Source %s   %s", wave_src_name(U.wave_src), st);
    gfx_text(4, 302, "KEY: next page   hold: change source", C_DIM, GFX_TRANSPARENT, 0);
}

/* ---------------- 统计页 ---------------- */
static int kv_y;
static void kv(int x, const char *k, uint16_t c, const char *fmt, ...)
{
    char buf[48];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gfx_text(x, kv_y, k, C_DIM, GFX_TRANSPARENT, 0);
    gfx_text(x + 80, kv_y, buf, c, GFX_TRANSPARENT, 0);
    kv_y += 17;
}

static void draw_stats(void)
{
    acq_stats_t s;
    char a[24], b[24], c[24];
    uint32_t sf = fl_last_status();
    acq_stats(&s);
    gfx_fill(0, 22, LCD_W, LCD_H - 22, C_BG);
    gfx_vline(240, 28, LCD_H - 34, C_GRID);

    const int L = 6, R = 248;
    uint32_t v = fl_version();
    kv_y = 28;
    kv(L, "FPGA", fl_state() == FL_DISCOVER ? C_BAD : C_TEXT, "%s v%u.%u.%u", fl_state_name(),
       (unsigned)(v >> 16), (unsigned)(v >> 8) & 255, (unsigned)v & 255);
    fmt_ip(a, sizeof(a), g_set.fpga_ip);
    kv(L, "FPGA IP", C_TEXT, "%s", a);
    fmt_ip(a, sizeof(a), plat_local_ip());
    kv(L, "MCU IP", C_TEXT, "%s", a);
    {
        static const char *const sp[4] = {"down", "10M", "100M", "1000M"};
        kv(L, "Link", (sf & MCA_SF_LINK) ? C_TEXT : C_DIM, "MCU %dM FPGA %s", plat_link_speed(),
           sp[MCA_SF_SPEED(sf)]);
    }
    kv(L, "Real", C_TEXT, "%.2f s", s.real_s);
    if (app_preset() > 0) kv(L, "Live", C_TEXT, "%.2f / %.0f s", s.live_s, app_preset());
    else kv(L, "Live", C_TEXT, "%.2f s", s.live_s);
    kv(L, "Dead", s.dead_pct > 50 ? C_BAD : C_TEXT, "%.2f %%", s.dead_pct);
    fmt_si(a, sizeof(a), s.icr);
    fmt_si(b, sizeof(b), s.ocr);
    fmt_si(c, sizeof(c), s.rate);
    kv(L, "ICR", C_TEXT, "%s cps", a);
    kv(L, "OCR", C_TEXT, "%s cps", b);
    kv(L, "Rate(LT)", C_GOOD, "%s cps", c);
    kv(L, "Counts", C_TEXT, "%llu", (unsigned long long)s.counts);
    kv(L, "Overflow", C_TEXT, "%llu", (unsigned long long)g_spec.overflow);
    kv(L, "Pile-up", C_TEXT, "%llu", (unsigned long long)s.fpga_pileup);
    kv(L, "Dropped", s.fpga_dropped ? C_WARN : C_TEXT, "%llu", (unsigned long long)s.fpga_dropped);
    kv(L, "Lost pkt", s.lost_pkts ? C_WARN : C_TEXT, "%llu (%.2f%%)", (unsigned long long)s.lost_pkts,
       s.lost_pkt_pct);
    kv(L, "Loss fac", C_TEXT, "%.4f", s.loss_factor);
    {
        char f[32] = "";
        if (sf & MCA_SF_FIFO_OVF) strcat(f, "OVF ");
        if (sf & MCA_SF_ADC_SAT) strcat(f, "SAT ");
        if (sf & MCA_SF_BASELINE_BAD) strcat(f, "BASE ");
        if (sf & MCA_SF_INHIBIT) strcat(f, "INH ");
        kv(L, "Flags", f[0] ? C_BAD : C_DIM, "%s", f[0] ? f : "none");
    }

    kv_y = 28;
    if (g_set.test_src) kv(R, "Source", C_WARN, "TEST %.0fcps %.0f", g_set.tp_rate, g_set.tp_amp);
    else kv(R, "Source", C_TEXT, "ADC");
    kv(R, "Polarity", C_TEXT, "%s%s", g_set.polarity_neg ? "NEG" : "POS", g_set.pu_discard ? " PU-rej" : "");
    kv(R, "Tau", C_TEXT, "%.2f us", g_set.tau_us);
    kv(R, "Shaping", C_TEXT, "%.2f/%.2f us", g_set.rise_us, g_set.flat_us);
    kv(R, "Fast", C_TEXT, "%.2f/%.2f us", g_set.fast_us, g_set.gap_us);
    kv(R, "Thresh", C_TEXT, "%.1f LSB", g_set.thr_lsb);
    kv(R, "Channels", C_TEXT, "%u (E>>%u)", (unsigned)g_spec.nch, (unsigned)g_spec.shift);
    if (uncalibrated()) kv(R, "Cal", C_DIM, "none");
    else kv(R, "Cal", C_TEXT, "%.3g %+.4g %s", g_set.cal[0], g_set.cal[1], g_set.cal_unit);
    kv_y += 4;
    gfx_text(R, kv_y, "#  Centroid  FWHM%   Area", C_DIM, GFX_TRANSPARENT, 0);
    kv_y += 17;
    for (int k = 0; k < g_spec.npeaks && kv_y < LCD_H - 16; k++) {
        const peak_t *p = &g_spec.peaks[k];
        double x = uncalibrated() ? p->centroid : p->energy;
        double res = uncalibrated() ? (p->centroid > 0 ? 100 * p->fwhm / p->centroid : 0) : p->res_pct;
        fmt_si(a, sizeof(a), p->area);
        gfx_printf(R, kv_y, C_TEXT, GFX_TRANSPARENT, 0, "%d %9.2f %6.2f %7s", k + 1, x, res, a);
        kv_y += 17;
    }
}

/* ---------------- 按键与调度 ---------------- */
static void on_short(void) { ui_set_screen((U.screen + 1) % UI_NSCREENS); }

static void on_long(void)
{
    if (U.screen == UI_WAVE) {
        U.wave_src = (uint8_t)((U.wave_src + 1) & 3u);
        U.t_wave = plat_now_ms() - WAVE_MS;
    } else {
        app_clear();
    }
}

void ui_set_screen(int s)
{
    U.screen = s;
    U.t_draw = plat_now_ms() - REDRAW_MS;
    if (s == UI_WAVE) U.t_wave = plat_now_ms() - WAVE_MS;
}

int ui_screen(void) { return U.screen; }

void ui_draw(void)
{
    status_bar();
    switch (U.screen) {
    case UI_WAVE:  draw_wave(); break;
    case UI_STATS: draw_stats(); break;
    default:       draw_spectrum(); break;
    }
}

void ui_init(void)
{
    memset(&U, 0, sizeof(U));
    U.wave_src = 1;
    gfx_fill(0, 0, LCD_W, LCD_H, C_BG);
    ui_draw();
    plat_lcd_flush(0, 0, LCD_W, LCD_H);
    U.t_draw = plat_now_ms();
}

void ui_poll(uint32_t now)
{
    uint32_t k = plat_keys() & 1u;
    if (k != U.key_raw) { U.key_raw = k; U.t_key = now; }
    if (now - U.t_key >= DEBOUNCE_MS && U.key_stable != U.key_raw) {
        U.key_stable = U.key_raw;
        if (U.key_stable) { U.t_press = now; U.long_fired = 0; }
        else if (!U.long_fired) on_short();
    }
    if (U.key_stable && !U.long_fired && now - U.t_press >= LONG_MS) {
        U.long_fired = 1;
        on_long();
    }

    wave_service(now);

    if (now - U.t_draw >= REDRAW_MS && !plat_lcd_busy()) {
        U.t_draw = now;
        ui_draw();
        plat_lcd_flush(0, 0, LCD_W, LCD_H);
    }
}
