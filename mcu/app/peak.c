#include "peak.h"

#include <math.h>
#include <string.h>

#include "platform.h"

#define SQRT2PI 2.5066282746310002
#define PI      3.14159265358979323846

/* 工作区用 float，8192 道时共 96 KB */
PLAT_BIG_BSS static float s_bkg[SPEC_MAX_CH];
PLAT_BIG_BSS static float s_work[SPEC_MAX_CH];
PLAT_BIG_BSS static float s_net[SPEC_MAX_CH];

static double lls(double y) { return log(log(sqrt(y + 1.0) + 1.0) + 1.0); }
static double lls_inv(double v) { double t = exp(exp(v) - 1.0) - 1.0; return t * t - 1.0; }

void peak_snip(const uint32_t *h, int n, int iters, float *bkg, float *work)
{
    for (int i = 0; i < n; i++) bkg[i] = (float)lls((double)h[i]);
    /* 窗口由大到小（"减小窗口" SNIP），保留峰附近的平滑本底 */
    for (int p = iters; p >= 1; p--) {
        for (int i = p; i < n - p; i++) {
            float m = 0.5f * (bkg[i - p] + bkg[i + p]);
            work[i] = bkg[i] < m ? bkg[i] : m;
        }
        for (int i = p; i < n - p; i++) bkg[i] = work[i];
    }
    for (int i = 0; i < n; i++) bkg[i] = (float)lls_inv(bkg[i]);
}

/* 模型：A·exp(-(x-μ)²/2σ²) + b0 + b1·(x-x0) */
static void model(const double *q, double x, double x0, double *f, double *d)
{
    double u = (x - q[1]) / q[2];
    double g = exp(-0.5 * u * u);
    *f = q[0] * g + q[3] + q[4] * (x - x0);
    if (d) {
        d[0] = g;
        d[1] = q[0] * g * u / q[2];
        d[2] = q[0] * g * u * u / q[2];
        d[3] = 1.0;
        d[4] = x - x0;
    }
}

static double chi2(const uint32_t *h, int lo, int hi, const double *q, double x0)
{
    double s = 0;
    for (int i = lo; i <= hi; i++) {
        double f, y = h[i];
        model(q, i, x0, &f, 0);
        double w = 1.0 / (y > 1 ? y : 1);
        s += w * (y - f) * (y - f);
    }
    return s;
}

int peak_fit(const uint32_t *h, int n, double c0, double s0, int lo, int hi, peak_t *p)
{
    if (lo < 0) lo = 0;
    if (hi > n - 1) hi = n - 1;
    if (hi - lo < 6) return -1;
    double x0 = c0;
    /* 初值：两端平均做本底 */
    double bl = 0, br = 0;
    for (int i = 0; i < 2; i++) { bl += h[lo + i]; br += h[hi - i]; }
    bl /= 2; br /= 2;
    double q[5];
    q[4] = (br - bl) / (hi - lo);
    q[3] = bl + q[4] * (x0 - lo);
    q[0] = h[(int)(c0 + 0.5)] - q[3];
    if (q[0] <= 0) q[0] = 1;
    q[1] = c0;
    q[2] = s0 > 0.7 ? s0 : 0.7;

    double lambda = 1e-3;
    double c_old = chi2(h, lo, hi, q, x0);
    double jtj[25], jtr[5];
    int it;
    for (it = 0; it < 50; it++) {
        memset(jtj, 0, sizeof(jtj));
        memset(jtr, 0, sizeof(jtr));
        for (int i = lo; i <= hi; i++) {
            double f, d[5], y = h[i];
            model(q, i, x0, &f, d);
            double w = 1.0 / (y > 1 ? y : 1);
            for (int r = 0; r < 5; r++) {
                jtr[r] += w * d[r] * (y - f);
                for (int c = 0; c < 5; c++) jtj[r * 5 + c] += w * d[r] * d[c];
            }
        }
        double a[25], b[5], qn[5];
        memcpy(a, jtj, sizeof(a));
        memcpy(b, jtr, sizeof(b));
        for (int r = 0; r < 5; r++) a[r * 5 + r] *= 1.0 + lambda;
        if (solve_linear(a, b, 5)) return -1;
        for (int r = 0; r < 5; r++) qn[r] = q[r] + b[r];
        if (qn[2] < 0.3) qn[2] = 0.3;
        double c_new = chi2(h, lo, hi, qn, x0);
        if (c_new < c_old) {
            int small = fabs(c_old - c_new) < 1e-6 * (c_old + 1e-9);
            memcpy(q, qn, sizeof(q));
            c_old = c_new;
            lambda *= 0.3;
            if (small) break;
        } else {
            lambda *= 10;
            if (lambda > 1e8) break;
        }
    }
    /* 协方差：(JᵀWJ)⁻¹，逐列求解 */
    double cov[25];
    for (int c = 0; c < 5; c++) {
        double a[25], e[5] = {0};
        memcpy(a, jtj, sizeof(a));
        e[c] = 1.0;
        if (solve_linear(a, e, 5)) return -1;
        for (int r = 0; r < 5; r++) cov[r * 5 + c] = e[r];
    }
    int ndf = (hi - lo + 1) - 5;
    double scale = ndf > 0 ? c_old / ndf : 1.0;
    if (scale < 1.0) scale = 1.0;           /* 拟合优于泊松时不缩小误差 */
    if (q[0] <= 0 || q[2] <= 0 || q[1] < lo || q[1] > hi) return -1;

    p->centroid = q[1];
    p->sigma    = q[2];
    p->fwhm     = 2.3548200450309493 * q[2];
    p->height   = q[0];
    p->area     = SQRT2PI * q[0] * q[2];
    double var  = 2 * PI * (q[2] * q[2] * cov[0] + q[0] * q[0] * cov[12] + 2 * q[0] * q[2] * cov[2]);
    p->area_err = var > 0 ? sqrt(var * scale) : 0.0;
    p->bkg0     = q[3] + q[4] * (q[1] - x0);
    p->bkg1     = q[4];
    p->chi2ndf  = ndf > 0 ? c_old / ndf : 0.0;
    return 0;
}

int peak_search(const uint32_t *h, int n, peak_t *out, int max_peaks)
{
    if (n < 32 || n > SPEC_MAX_CH) return 0;
    /* SNIP 窗口要不小于最宽的峰（NaI 在 4096 道时 1.3 MeV 的 FWHM ≈ 120 道），否则会削掉峰的一部分 */
    int iters = n / 32;
    if (iters < 8) iters = 8;
    peak_snip(h, n, iters, s_bkg, s_work);

    /* 平滑净计数（5 点三角） */
    for (int i = 0; i < n; i++) {
        double s = 0, w = 0;
        for (int k = -2; k <= 2; k++) {
            int j = i + k;
            if (j < 0 || j >= n) continue;
            double wk = 3 - (k < 0 ? -k : k);
            s += wk * ((double)h[j] - s_bkg[j]);
            w += wk;
        }
        s_net[i] = (float)(s / w);
    }

    peak_t cand[32];
    int nc = 0;
    int tops[64], nt = 0;                        /* 已拟合过的峰顶 */
    for (int i = 3; i < n - 3 && nc < 32; i++) {
        double v = s_net[i];
        if (v <= 0) continue;
        int is_max = 1;
        for (int k = -3; k <= 3; k++)
            if (k && s_net[i + k] > v) { is_max = 0; break; }
        if (!is_max) continue;
        /* 显著性：净计数超过本底涨落 4σ 且至少 10 计数 */
        if (v < 10 || v < 4.0 * sqrt(s_bkg[i] + v)) continue;
        /* 宽峰计数少时，噪声会在峰的前沿、康普顿平台上造出局部极大；爬到半高区间内的最高点当峰顶，
         * 再量半高宽。同一个峰顶只拟合一次 */
        int t = i, l, r;
        for (;;) {
            l = r = t;
            while (l > 0 && s_net[l] > s_net[t] / 2) l--;
            while (r < n - 1 && s_net[r] > s_net[t] / 2) r++;
            int m = t;
            for (int k = l; k <= r; k++)
                if (s_net[k] > s_net[m]) m = k;
            if (m == t) break;
            t = m;
        }
        int seen = 0;
        for (int k = 0; k < nt && !seen; k++) seen = tops[k] == t;
        if (seen) continue;
        if (nt < 64) tops[nt++] = t;
        double fwhm = r - l;
        if (fwhm < 2) fwhm = 2;
        double sg = fwhm / 2.3548;
        int lo = (int)floor(t - 2.5 * sg), hi = (int)ceil(t + 2.5 * sg);
        if (hi - lo < 8) { lo = t - 4; hi = t + 4; }
        peak_t p;
        if (peak_fit(h, n, t, sg, lo, hi, &p)) continue;
        /* 按拟合出的 σ 重定 ROI（±3σ）再拟合，结果不再依赖粗估的半高宽 */
        for (int it = 0; it < 2; it++) {
            peak_t q;
            if (p.sigma <= 0.5 || p.sigma > 0.25 * n) break;
            int l2 = (int)floor(p.centroid - 3 * p.sigma), h2 = (int)ceil(p.centroid + 3 * p.sigma);
            if (l2 < 0) l2 = 0;
            if (h2 > n - 1) h2 = n - 1;
            if (h2 - l2 < 8 || peak_fit(h, n, p.centroid, p.sigma, l2, h2, &q)) break;
            p = q;
        }
        /* 峰位要落在峰顶的半高区间内，否则是拟合跑到了本底或邻峰上 */
        if (p.area < 3 * p.area_err || p.sigma > 0.25 * n || p.centroid < l || p.centroid > r) continue;
        /* 去重 */
        int dup = 0;
        for (int k = 0; k < nc; k++)
            if (fabs(cand[k].centroid - p.centroid) < 1.5 * (cand[k].sigma > p.sigma ? cand[k].sigma : p.sigma)) {
                if (p.area > cand[k].area) cand[k] = p;
                dup = 1;
                break;
            }
        if (!dup) cand[nc++] = p;
    }
    /* 按面积排序 */
    for (int a = 0; a < nc; a++)
        for (int b = a + 1; b < nc; b++)
            if (cand[b].area > cand[a].area) { peak_t t = cand[a]; cand[a] = cand[b]; cand[b] = t; }
    if (nc > max_peaks) nc = max_peaks;
    memcpy(out, cand, sizeof(peak_t) * (size_t)nc);
    return nc;
}
