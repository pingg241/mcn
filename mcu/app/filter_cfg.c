#include "filter_cfg.h"

#include <math.h>

#include "mca_proto.h"
#include "settings.h"

static long clampl(long v, long lo, long hi) { return v < lo ? lo : (v > hi ? hi : v); }

void filter_params(double tau_us, double rise_us, double flat_us, double fast_us, double gap_us,
                   double thr_lsb, double full_scale, filter_regs_t *r)
{
    const double ts = 1.0 / MCA_FS_HZ;
    long k   = clampl(lround(rise_us * 1e-6 / ts), 1, 1023);
    long m   = clampl(lround(flat_us * 1e-6 / ts), 0, 255);
    long kf  = clampl(lround(fast_us * 1e-6 / ts), 1, 63);
    long gap = clampl(lround(gap_us * 1e-6 / ts), 0, 63);
    /* M = 1/(e^{Ts/τ} - 1)，Q16.8 */
    double M = 1.0 / expm1(ts / (tau_us * 1e-6));
    long mq  = clampl(lround(M * 256.0), 0, 0xFFFFFF);
    double m_eff = mq / 256.0;
    /* 平顶 H = A·k·(M+1)·256；E = ((H >>> S)·G) >> 14，A = 255 → full_scale */
    double ratio = full_scale / 255.0 / ((double)k * (m_eff + 1.0) * 256.0);
    long shift = 0, gain = 0;
    for (int s = 0; s < 48; s++) {
        double g = ratio * ldexp(1.0, s + 14);
        if (g >= 65535.5) break;
        shift = s;
        gain  = lround(g);
    }
    r->fast_k       = (uint32_t)kf;
    r->fast_gap     = (uint32_t)gap;
    r->threshold    = (uint32_t)clampl(lround(thr_lsb * kf), 1, 65535);
    r->slow_k       = (uint32_t)k;
    r->slow_m       = (uint32_t)m;
    r->pz_m         = (uint32_t)mq;
    r->peak_delay   = (uint32_t)clampl(k + 7 - kf, 0, 4095);
    r->peak_win     = (uint32_t)(m + kf + 2);
    r->pileup_win   = (uint32_t)clampl(k + m + kf + 8, 1, 4095);
    r->blr_hold     = (uint32_t)clampl(2 * k + m + kf + 16, 0, 4095);
    r->energy_gain  = (uint32_t)gain;
    r->energy_shift = (uint32_t)shift;
}

int filter_cfg_build(reg_write_t *o, int max)
{
    filter_regs_t r;
    filter_params(g_set.tau_us, g_set.rise_us, g_set.flat_us, g_set.fast_us, g_set.gap_us,
                  g_set.thr_lsb, 65280.0, &r);
    int n = 0;
#define W(a, v) do { if (n < max) { o[n].addr = (a); o[n].value = (v); n++; } } while (0)
    W(MCA_REG_FAST_K, r.fast_k);
    W(MCA_REG_FAST_GAP, r.fast_gap);
    W(MCA_REG_THRESHOLD, r.threshold);
    W(MCA_REG_SLOW_K, r.slow_k);
    W(MCA_REG_SLOW_M, r.slow_m);
    W(MCA_REG_PZ_M, r.pz_m);
    W(MCA_REG_PEAK_DELAY, r.peak_delay);
    W(MCA_REG_PEAK_WIN, r.peak_win);
    W(MCA_REG_PILEUP_WIN, r.pileup_win);
    W(MCA_REG_BLR_HOLD, r.blr_hold);
    W(MCA_REG_ENERGY_GAIN, r.energy_gain);
    W(MCA_REG_ENERGY_SHIFT, r.energy_shift);
    if (g_set.test_src) {
        /* 测试源衰减 b = 1 - 2^-decay，取最接近 τ 的 decay */
        double tau_smp = g_set.tau_us * 1e-6 * MCA_FS_HZ;
        long decay = clampl(lround(log2(tau_smp)), 1, 15);
        W(MCA_REG_TP_RATE, (uint32_t)llround(g_set.tp_rate / MCA_FS_HZ * 4294967296.0));
        W(MCA_REG_TP_AMP, (uint32_t)lround(g_set.tp_amp * 256.0));
        W(MCA_REG_TP_CFG, (uint32_t)((3u << 12) | ((uint32_t)decay << 8) | (1u << 4) | 1u));
    }
#undef W
    return n;
}

uint32_t filter_cfg_ctrl(int run)
{
    return (run ? MCA_CTRL_RUN : 0u) | (g_set.test_src ? MCA_CTRL_SRC_TEST : 0u) |
           (g_set.polarity_neg ? MCA_CTRL_POL_NEG : 0u) | (g_set.pu_discard ? MCA_CTRL_PU_DISCARD : 0u);
}
