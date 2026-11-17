#include "fpga_link.h"

#include <string.h>

#include "filter_cfg.h"
#include "platform.h"
#include "settings.h"

#define QLEN          32
#define CMD_TIMEOUT   100     /* ms */
#define CMD_RETRIES   3
#define PING_PERIOD   500
#define HB_TIMEOUT    3500    /* 收不到心跳视为 FPGA 失联 */

typedef struct {
    uint8_t  cmd;
    uint32_t addr, value;
    fl_cb_t  cb;
    void    *ctx;
} qent_t;

static struct {
    fl_state_t st;
    qent_t   q[QLEN];
    unsigned head, tail;          /* q[head] 为正在发送的命令 */
    int      inflight;
    uint8_t  seq;
    uint8_t  tries;
    uint32_t t_sent;
    uint32_t t_ping;
    uint32_t t_hb;
    int      have_hb;
    mca_mcst_t last;
    uint32_t version;
    uint32_t timeouts;
    int      run_wanted;
} L;

static unsigned qcount(void) { return (L.tail - L.head) % QLEN; }

static void send_head(uint32_t now)
{
    uint8_t buf[16];
    qent_t *e = &L.q[L.head];
    L.seq++;
    mca_build_mccm(buf, e->cmd, L.seq, e->addr, e->value);
    plat_udp_send(MCA_PORT_CMD, g_set.fpga_ip, MCA_PORT_CMD, buf, sizeof(buf));
    L.t_sent   = now;
    L.inflight = 1;
}

static void flush_queue(uint8_t status)
{
    while (qcount()) {
        qent_t e = L.q[L.head];
        L.head = (L.head + 1) % QLEN;
        if (e.cb) e.cb(e.ctx, status, 0);
    }
    L.inflight = 0;
}

int fl_cmd(uint8_t cmd, uint32_t addr, uint32_t value, fl_cb_t cb, void *ctx)
{
    if (qcount() >= QLEN - 1) return -1;
    qent_t *e = &L.q[L.tail];
    e->cmd = cmd; e->addr = addr; e->value = value; e->cb = cb; e->ctx = ctx;
    L.tail = (L.tail + 1) % QLEN;
    return 0;
}

static void enter(fl_state_t s)
{
    if (s != L.st) plat_log("fpga_link: %s -> %s\n", fl_state_name(), s == FL_DISCOVER ? "DISCOVER" :
                                                                       s == FL_CONFIG ? "CONFIG" : "RUN");
    L.st = s;
    if (s == FL_DISCOVER) {
        flush_queue(MCA_ST_TIMEOUT);
        L.have_hb = 0;
        L.t_ping  = plat_now_ms() - PING_PERIOD;
    }
}

/* ---------------- 配置序列 ---------------- */
static void cfg_done(void *ctx, uint8_t st, uint32_t v)
{
    (void)ctx; (void)v;
    if (st == MCA_ST_OK) {
        L.t_hb = plat_now_ms();
        enter(FL_RUN);
    }
}

static void cleared_cb(void *ctx, uint8_t st, uint32_t v)
{
    (void)ctx; (void)v;
    if (st == MCA_ST_OK) app_on_fpga_cleared();
}

static void cfg_step_cb(void *ctx, uint8_t st, uint32_t v)
{
    (void)ctx; (void)v;
    if (st != MCA_ST_OK) plat_log("fpga_link: 配置命令失败 %s\n", mca_status_name(st));
}

void fl_reconfigure(void)
{
    reg_write_t w[FILTER_CFG_MAX];
    int n = filter_cfg_build(w, FILTER_CFG_MAX);
    flush_queue(MCA_ST_TIMEOUT);
    L.st = FL_CONFIG;
    fl_cmd(MCA_CMD_WRITE_REG, MCA_REG_CTRL, 0, cfg_step_cb, 0);
    for (int i = 0; i < n; i++) fl_cmd(MCA_CMD_WRITE_REG, w[i].addr, w[i].value, cfg_step_cb, 0);
    fl_cmd(MCA_CMD_SET_STREAM, g_set.stream_n, g_set.stream_ms, cfg_step_cb, 0);
    fl_cmd(MCA_CMD_CLEAR_COUNTERS, 0, 0, cleared_cb, 0);
    fl_cmd(MCA_CMD_WRITE_REG, MCA_REG_CTRL, filter_cfg_ctrl(L.run_wanted), cfg_done, 0);
}

void fl_set_run(int run)
{
    L.run_wanted = run;
    if (L.st != FL_DISCOVER) fl_cmd(MCA_CMD_WRITE_REG, MCA_REG_CTRL, filter_cfg_ctrl(run), 0, 0);
}

void fl_clear(void)
{
    if (L.st == FL_DISCOVER) app_on_fpga_cleared();
    else fl_cmd(MCA_CMD_CLEAR_COUNTERS, 0, 0, cleared_cb, 0);
}

/* ---------------- 接收 ---------------- */
static void ping_cb(void *ctx, uint8_t st, uint32_t v)
{
    (void)ctx;
    if (st == MCA_ST_OK && L.st == FL_DISCOVER) {
        L.version = v;
        plat_log("fpga_link: FPGA 应答，版本 %u.%u.%u\n", (unsigned)(v >> 16), (unsigned)(v >> 8) & 255,
                 (unsigned)v & 255);
        fl_reconfigure();
    }
}

void fl_on_mcrs(const mca_mcrs_t *r)
{
    if (!L.inflight || r->seq != L.seq || r->cmd != L.q[L.head].cmd) return;
    qent_t e = L.q[L.head];
    L.head = (L.head + 1) % QLEN;
    L.inflight = 0;
    L.tries = 0;
    if (e.cb) e.cb(e.ctx, r->status, r->value);
}

void fl_on_mcst(const mca_mcst_t *s)
{
    uint32_t now = plat_now_ms();
    if (L.have_hb && L.st == FL_RUN &&
        (s->total_ticks < L.last.total_ticks || (int32_t)(s->seq - L.last.seq) < 0)) {
        plat_log("fpga_link: 检测到 FPGA 复位，重新配置\n");
        L.have_hb = 0;
        fl_reconfigure();
    }
    L.last    = *s;
    L.have_hb = 1;
    L.t_hb    = now;
}

/* ---------------- 轮询 ---------------- */
void fl_poll(uint32_t now)
{
    if (L.inflight && now - L.t_sent >= CMD_TIMEOUT) {
        if (++L.tries < CMD_RETRIES) {
            send_head(now);
        } else {
            qent_t e = L.q[L.head];
            L.head = (L.head + 1) % QLEN;
            L.inflight = 0;
            L.tries = 0;
            L.timeouts++;
            if (L.st != FL_DISCOVER) plat_log("fpga_link: %s 超时\n", mca_cmd_name(e.cmd));
            if (e.cb) e.cb(e.ctx, MCA_ST_TIMEOUT, 0);
            enter(FL_DISCOVER);
        }
    }
    switch (L.st) {
    case FL_DISCOVER:
        if (!qcount() && now - L.t_ping >= PING_PERIOD && plat_link_speed() > 0) {
            L.t_ping = now;
            fl_cmd(MCA_CMD_PING, 0, 0, ping_cb, 0);
        }
        break;
    case FL_RUN:
        if (now - L.t_hb > HB_TIMEOUT) {
            plat_log("fpga_link: 心跳超时\n");
            enter(FL_DISCOVER);
        }
        break;
    default:
        break;
    }

    if (!L.inflight && qcount()) {
        L.tries = 0;
        send_head(now);
    }
}

void fl_init(void)
{
    memset(&L, 0, sizeof(L));
    L.run_wanted = 1;
    L.st = FL_DISCOVER;
    L.t_ping = plat_now_ms() - PING_PERIOD;
}

fl_state_t fl_state(void) { return L.st; }
const char *fl_state_name(void)
{
    return L.st == FL_DISCOVER ? "DISCOVER" : L.st == FL_CONFIG ? "CONFIG" : "RUN";
}
int fl_running(void) { return L.st == FL_RUN && L.have_hb && (L.last.status_flags & MCA_SF_RUN); }
int fl_run_wanted(void) { return L.run_wanted; }
uint32_t fl_last_status(void) { return L.have_hb ? L.last.status_flags : 0; }
uint32_t fl_version(void) { return L.version; }
uint32_t fl_timeouts(void) { return L.timeouts; }
