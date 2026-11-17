/* 物理参数 → FPGA 寄存器（与 tools/mcaproto.py filter_params 一致） */
#ifndef FILTER_CFG_H
#define FILTER_CFG_H

#include <stdint.h>

typedef struct {
    uint32_t addr, value;
} reg_write_t;

#define FILTER_CFG_MAX 24

/* 按 g_set 生成写寄存器序列（不含 CTRL），返回条数 */
int filter_cfg_build(reg_write_t *out, int max);
/* 由 g_set 算 CTRL（含 RUN 位） */
uint32_t filter_cfg_ctrl(int run);

typedef struct {
    uint32_t fast_k, fast_gap, threshold, slow_k, slow_m, pz_m;
    uint32_t peak_delay, peak_win, pileup_win, blr_hold, energy_gain, energy_shift;
} filter_regs_t;

void filter_params(double tau_us, double rise_us, double flat_us, double fast_us, double gap_us,
                   double thr_lsb, double full_scale, filter_regs_t *r);

#endif
