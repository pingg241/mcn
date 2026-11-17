/* 能谱直方图、能量刻度、寻峰结果 */
#ifndef SPECTRUM_H
#define SPECTRUM_H

#include <stdint.h>

#define SPEC_MAX_CH    8192
#define SPEC_MAX_PEAKS 8

typedef struct {
    double centroid;     /* 道址 */
    double sigma;        /* 道 */
    double fwhm;         /* 道 */
    double area;         /* 净面积 */
    double area_err;
    double height;
    double bkg0, bkg1;   /* 本底 b0 + b1·(x - centroid) */
    double chi2ndf;
    double energy;       /* 刻度后能量 */
    double fwhm_e;       /* 刻度后 FWHM */
    double res_pct;      /* FWHM/E × 100 */
} peak_t;

typedef struct {
    uint32_t nch;
    uint32_t shift;               /* 道址 = energy >> shift */
    uint32_t h[SPEC_MAX_CH];
    uint64_t total;               /* 进谱计数 */
    uint64_t overflow;            /* 超出道数的计数 */
    peak_t   peaks[SPEC_MAX_PEAKS];
    int      npeaks;
} spectrum_t;

extern spectrum_t g_spec;

void spec_reset(uint32_t nch, uint32_t shift);
static inline void spec_add(uint16_t energy)
{
    uint32_t ch = (uint32_t)energy >> g_spec.shift;
    if (ch < g_spec.nch) { g_spec.h[ch]++; g_spec.total++; }
    else g_spec.overflow++;
}

/* 刻度：E = c0 + c1·ch + c2·ch² */
double spec_ch2e(double ch);
double spec_e2ch(double e);
/* 由 n 个 (道址, 能量) 点最小二乘拟合刻度（n=1 仅比例，n=2 线性，n≥3 二次），返回 0 成功 */
int spec_calibrate(const double *ch, const double *e, int n, float coef[3]);

/* 重新寻峰并拟合，结果写入 g_spec.peaks */
int spec_analyze(void);

#endif
