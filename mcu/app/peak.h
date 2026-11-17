/* 寻峰：SNIP 本底 + 显著性判别 + 高斯/线性本底 Levenberg-Marquardt 拟合 */
#ifndef PEAK_H
#define PEAK_H

#include <stdint.h>

#include "spectrum.h"

/* 在直方图 h[0..n) 中找峰，按面积从大到小写入 out，返回个数 */
int peak_search(const uint32_t *h, int n, peak_t *out, int max_peaks);

/* 以 (c0, s0) 为初值在 [lo, hi] 拟合单峰，成功返回 0 */
int peak_fit(const uint32_t *h, int n, double c0, double s0, int lo, int hi, peak_t *p);

/* SNIP 本底（LLS 变换），bkg 与 h 等长，work 为 n 个 float 的临时区 */
void peak_snip(const uint32_t *h, int n, int iters, float *bkg, float *work);

int solve_linear(double *a, double *b, int n);

#endif
