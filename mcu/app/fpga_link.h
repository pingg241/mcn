/* FPGA 命令通道：命令队列（序号、超时重发）、发现 → 配置 → 运行状态机、心跳看门狗 */
#ifndef FPGA_LINK_H
#define FPGA_LINK_H

#include <stdint.h>

#include "mca_proto.h"

typedef enum { FL_DISCOVER = 0, FL_CONFIG, FL_RUN } fl_state_t;

/* 命令完成回调：status 为 MCA_ST_*（超时为 MCA_ST_TIMEOUT） */
typedef void (*fl_cb_t)(void *ctx, uint8_t status, uint32_t value);

void fl_init(void);
void fl_poll(uint32_t now_ms);
void fl_on_mcrs(const mca_mcrs_t *r);
void fl_on_mcst(const mca_mcst_t *s);

/* 排队发送一条命令，返回 0 成功，队列满返回 -1 */
int fl_cmd(uint8_t cmd, uint32_t addr, uint32_t value, fl_cb_t cb, void *ctx);

void fl_reconfigure(void);     /* 按 g_set 重新下发全部配置 */
void fl_set_run(int run);      /* 启停采集（写 CTRL） */
void fl_clear(void);           /* 清 FPGA 计数器，完成后回调 app 清能谱 */

fl_state_t  fl_state(void);
const char *fl_state_name(void);
int         fl_running(void);
int         fl_run_wanted(void);    /* 用户要求的启停状态 */
uint32_t    fl_last_status(void);   /* 最近一次心跳的 status_flags */
uint32_t    fl_version(void);
uint32_t    fl_timeouts(void);

/* 由 app 实现：FPGA 计数器已清零 */
void app_on_fpga_cleared(void);

#endif
