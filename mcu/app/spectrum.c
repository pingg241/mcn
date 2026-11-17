#include "spectrum.h"

#include <math.h>
#include <string.h>

#include "peak.h"
#include "settings.h"

spectrum_t g_spec;

void spec_reset(uint32_t nch, uint32_t shift)
{
    if (nch > SPEC_MAX_CH) nch = SPEC_MAX_CH;
    memset(g_spec.h, 0, sizeof(g_spec.h));
    g_spec.nch = nch;
    g_spec.shift = shift;
    g_spec.total = 0;
    g_spec.overflow = 0;
    g_spec.npeaks = 0;
}

double spec_ch2e(double ch)
{
    return g_set.cal[0] + g_set.cal[1] * ch + g_set.cal[2] * ch * ch;
}

double spec_e2ch(double e)
{
    double a = g_set.cal[2], b = g_set.cal[1], c = g_set.cal[0] - e;
    if (fabs(a) < 1e-12) return fabs(b) > 1e-12 ? -c / b : 0.0;
    double d = b * b - 4 * a * c;
    if (d < 0) return 0.0;
    return (-b + sqrt(d)) / (2 * a);
}

/* 小规模线性方程组（高斯消元，列主元） */
int solve_linear(double *a, double *b, int n)
{
    for (int c = 0; c < n; c++) {
        int p = c;
        for (int r = c + 1; r < n; r++)
            if (fabs(a[r * n + c]) > fabs(a[p * n + c])) p = r;
        if (fabs(a[p * n + c]) < 1e-300) return -1;
        if (p != c) {
            for (int k = 0; k < n; k++) {
                double t = a[c * n + k]; a[c * n + k] = a[p * n + k]; a[p * n + k] = t;
            }
            double t = b[c]; b[c] = b[p]; b[p] = t;
        }
        for (int r = c + 1; r < n; r++) {
            double f = a[r * n + c] / a[c * n + c];
            for (int k = c; k < n; k++) a[r * n + k] -= f * a[c * n + k];
            b[r] -= f * b[c];
        }
    }
    for (int r = n - 1; r >= 0; r--) {
        double s = b[r];
        for (int k = r + 1; k < n; k++) s -= a[r * n + k] * b[k];
        b[r] = s / a[r * n + r];
    }
    return 0;
}

int spec_calibrate(const double *ch, const double *e, int n, float coef[3])
{
    if (n < 1) return -1;
    if (n == 1) {
        if (ch[0] <= 0) return -1;
        coef[0] = 0; coef[1] = (float)(e[0] / ch[0]); coef[2] = 0;
        return 0;
    }
    int m = n >= 3 ? 3 : 2;
    double a[9] = {0}, b[3] = {0};
    for (int i = 0; i < n; i++) {
        double xp[3] = {1.0, ch[i], ch[i] * ch[i]};
        for (int r = 0; r < m; r++) {
            b[r] += xp[r] * e[i];
            for (int c = 0; c < m; c++) a[r * m + c] += xp[r] * xp[c];
        }
    }
    if (solve_linear(a, b, m)) return -1;
    coef[0] = (float)b[0];
    coef[1] = (float)b[1];
    coef[2] = m == 3 ? (float)b[2] : 0.0f;
    return 0;
}

int spec_analyze(void)
{
    peak_t pk[SPEC_MAX_PEAKS];
    int n = peak_search(g_spec.h, (int)g_spec.nch, pk, SPEC_MAX_PEAKS);
    for (int i = 0; i < n; i++) {
        pk[i].energy = spec_ch2e(pk[i].centroid);
        pk[i].fwhm_e = fabs(spec_ch2e(pk[i].centroid + pk[i].fwhm / 2) - spec_ch2e(pk[i].centroid - pk[i].fwhm / 2));
        pk[i].res_pct = pk[i].energy > 0 ? 100.0 * pk[i].fwhm_e / pk[i].energy : 0.0;
    }
    memcpy(g_spec.peaks, pk, sizeof(peak_t) * (size_t)n);
    g_spec.npeaks = n;
    return n;
}
