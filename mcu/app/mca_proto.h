/* MCA 协议定义（与 docs/protocol.md、tools/mcaproto.py、fpga/rtl/mca_regs.vh 一致）
 * 大端字节序；包以 4 字节 ASCII magic 开头；时间戳 32 MHz 采样计数。 */
#ifndef MCA_PROTO_H
#define MCA_PROTO_H

#include <stddef.h>
#include <stdint.h>

#define MCA_FS_HZ        32000000u
#define MCA_PORT_CMD     5000u      /* MCCM/MCRS/MCST */
#define MCA_PORT_EV      5001u      /* MCEV/MCWF */
#define MCA_PORT_CONSOLE 6000u      /* MCU 文本控制台（本工程扩展） */

/* 命令码 */
enum {
    MCA_CMD_PING = 0x01, MCA_CMD_WRITE_REG, MCA_CMD_READ_REG, MCA_CMD_CLEAR_COUNTERS,
    MCA_CMD_SET_STREAM, MCA_CMD_CAPTURE_WAVE, MCA_CMD_GET_STATUS, MCA_CMD_RESET, MCA_CMD_SET_NET
};
/* 应答状态 */
enum { MCA_ST_OK = 0, MCA_ST_BAD_CMD, MCA_ST_BAD_ADDR, MCA_ST_BAD_VALUE, MCA_ST_BUSY, MCA_ST_TIMEOUT = 0xFF };

/* 寄存器地址 */
enum {
    MCA_REG_ID = 0x00, MCA_REG_VERSION = 0x04, MCA_REG_CTRL = 0x08, MCA_REG_STATUS = 0x0C,
    MCA_REG_FAST_K = 0x10, MCA_REG_FAST_GAP = 0x14, MCA_REG_THRESHOLD = 0x18,
    MCA_REG_SLOW_K = 0x20, MCA_REG_SLOW_M = 0x24, MCA_REG_PZ_M = 0x28,
    MCA_REG_PEAK_DELAY = 0x2C, MCA_REG_PEAK_WIN = 0x30, MCA_REG_BLR_SHIFT = 0x34,
    MCA_REG_PILEUP_WIN = 0x38, MCA_REG_BLR_HOLD = 0x3C,
    MCA_REG_ENERGY_GAIN = 0x40, MCA_REG_ENERGY_SHIFT = 0x44, MCA_REG_BL_WIN = 0x48, MCA_REG_TOT_MAX = 0x4C,
    MCA_REG_TP_RATE = 0x50, MCA_REG_TP_AMP = 0x54, MCA_REG_TP_CFG = 0x58, MCA_REG_TP_BASE = 0x5C,
    MCA_REG_TOTAL_EVENTS_LO = 0x100, MCA_REG_TOTAL_EVENTS_HI = 0x104,
    MCA_REG_PILEUP_EVENTS = 0x108, MCA_REG_DROPPED_EVENTS = 0x10C,
    MCA_REG_TOTAL_TICKS_LO = 0x110, MCA_REG_TOTAL_TICKS_HI = 0x114,
    MCA_REG_BUSY_TICKS_LO = 0x118, MCA_REG_BUSY_TICKS_HI = 0x11C,
    MCA_REG_ADC_SAT_EVENTS = 0x120, MCA_REG_BASELINE = 0x124,
    MCA_REG_TIMESTAMP_LO = 0x128, MCA_REG_TIMESTAMP_HI = 0x12C,
    MCA_REG_PKT_SEQ = 0x130, MCA_REG_NET_STATUS = 0x134, MCA_REG_STREAM_N = 0x138,
    MCA_REG_STREAM_MS = 0x13C, MCA_REG_LOCAL_IP = 0x140, MCA_REG_HOST_IP = 0x144,
    MCA_REG_RX_FRAMES = 0x148, MCA_REG_RX_ERRORS = 0x14C
};

#define MCA_CTRL_RUN        0x01u
#define MCA_CTRL_SRC_TEST   0x02u
#define MCA_CTRL_POL_NEG    0x04u
#define MCA_CTRL_PU_DISCARD 0x08u
#define MCA_CTRL_PEAK_FIXED 0x10u

/* 事件 flags */
#define MCA_EV_SAT      0x01u
#define MCA_EV_BASELINE 0x02u
#define MCA_EV_TEST     0x04u
#define MCA_EV_PILEUP   0x08u

/* status_flags */
#define MCA_SF_RUN          (1u << 0)
#define MCA_SF_LINK         (1u << 1)
#define MCA_SF_SPEED(x)     (((x) >> 2) & 3u)      /* 0 无 1 10M 2 100M 3 1000M */
#define MCA_SF_HOST_VALID   (1u << 4)
#define MCA_SF_SRC_TEST     (1u << 5)
#define MCA_SF_WAVE_BUSY    (1u << 6)
#define MCA_SF_FIFO_OVF     (1u << 7)
#define MCA_SF_ADC_SAT      (1u << 8)
#define MCA_SF_BASELINE_BAD (1u << 9)
#define MCA_SF_PU_DISCARD   (1u << 10)
#define MCA_SF_INHIBIT      (1u << 11)
#define MCA_SF_HOST_LOCK    (1u << 12)

#define MCA_MCEV_MAX_EVENTS 64
#define MCA_MCWF_MAX_SAMPLES 256

typedef struct {
    uint64_t ts;        /* 48 位：ts_epoch:timestamp_low */
    uint16_t energy;
    uint8_t  flags;
} mca_event_t;

typedef struct {
    uint16_t n;
    uint32_t seq;
    uint16_t epoch;
    const uint8_t *rec;  /* 指向原始事件记录，用 mca_mcev_event() 取出 */
} mca_mcev_t;

typedef struct {
    uint32_t seq, pkt_seq;
    uint64_t total_events;
    uint32_t pileup_events, dropped_events;
    uint64_t total_ticks, busy_ticks;
    uint32_t adc_sat_events, status_flags;
} mca_mcst_t;

typedef struct {
    uint8_t  cmd, seq, status;
    uint32_t value;
} mca_mcrs_t;

typedef struct {
    uint8_t  src;
    uint16_t n;
    uint32_t capture_id;
    uint16_t offset, total;
    uint32_t trig_ts;
    uint16_t pretrig;
    const uint8_t *samples;   /* 大端 int16 × n */
} mca_mcwf_t;

typedef enum { MCA_PKT_NONE = 0, MCA_PKT_MCEV, MCA_PKT_MCST, MCA_PKT_MCRS, MCA_PKT_MCWF } mca_pkt_type_t;

/* 按 magic 判断包类型并检查长度 */
mca_pkt_type_t mca_pkt_type(const uint8_t *p, size_t len);
int  mca_parse_mcev(const uint8_t *p, size_t len, mca_mcev_t *out);
void mca_mcev_event(const mca_mcev_t *m, unsigned i, mca_event_t *ev);
int  mca_parse_mcst(const uint8_t *p, size_t len, mca_mcst_t *out);
int  mca_parse_mcrs(const uint8_t *p, size_t len, mca_mcrs_t *out);
int  mca_parse_mcwf(const uint8_t *p, size_t len, mca_mcwf_t *out);
int16_t mca_mcwf_sample(const mca_mcwf_t *w, unsigned i);

/* 生成 16 字节 MCCM 命令包，返回长度 */
size_t mca_build_mccm(uint8_t *buf, uint8_t cmd, uint8_t seq, uint32_t addr, uint32_t value);

/* 大端读写 */
static inline uint16_t mca_be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t mca_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline uint64_t mca_be64(const uint8_t *p) { return ((uint64_t)mca_be32(p) << 32) | mca_be32(p + 4); }
static inline void mca_put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void mca_put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* CRC-32（反射多项式 0xEDB88320），crc 传 0 开始，可分段累加 */
uint32_t mca_crc32(uint32_t crc, const void *data, size_t n);

const char *mca_cmd_name(uint8_t cmd);
const char *mca_status_name(uint8_t st);

#endif
