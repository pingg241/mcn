#include "mca_proto.h"

#include <string.h>

static int magic_is(const uint8_t *p, const char *m) { return memcmp(p, m, 4) == 0; }

mca_pkt_type_t mca_pkt_type(const uint8_t *p, size_t len)
{
    if (len < 16) return MCA_PKT_NONE;
    if (magic_is(p, "MCEV")) {
        unsigned n = mca_be16(p + 6);
        return (n <= MCA_MCEV_MAX_EVENTS && len >= 16u + 8u * n) ? MCA_PKT_MCEV : MCA_PKT_NONE;
    }
    if (magic_is(p, "MCST")) return len >= 64 ? MCA_PKT_MCST : MCA_PKT_NONE;
    if (magic_is(p, "MCRS")) return MCA_PKT_MCRS;
    if (magic_is(p, "MCWF")) {
        unsigned n = len >= 24 ? mca_be16(p + 6) : 0;
        return (len >= 24 && n <= MCA_MCWF_MAX_SAMPLES && len >= 24u + 2u * n) ? MCA_PKT_MCWF : MCA_PKT_NONE;
    }
    return MCA_PKT_NONE;
}

int mca_parse_mcev(const uint8_t *p, size_t len, mca_mcev_t *out)
{
    if (mca_pkt_type(p, len) != MCA_PKT_MCEV) return -1;
    out->n     = mca_be16(p + 6);
    out->seq   = mca_be32(p + 8);
    out->epoch = mca_be16(p + 12);
    out->rec   = p + 16;
    return 0;
}

void mca_mcev_event(const mca_mcev_t *m, unsigned i, mca_event_t *ev)
{
    const uint8_t *r = m->rec + 8u * i;
    ev->ts     = ((uint64_t)m->epoch << 32) | mca_be32(r);
    ev->energy = mca_be16(r + 4);
    ev->flags  = r[6];
}

int mca_parse_mcst(const uint8_t *p, size_t len, mca_mcst_t *o)
{
    if (mca_pkt_type(p, len) != MCA_PKT_MCST) return -1;
    o->seq            = mca_be32(p + 8);
    o->pkt_seq        = mca_be32(p + 12);
    o->total_events   = mca_be64(p + 16);
    o->pileup_events  = mca_be32(p + 24);
    o->dropped_events = mca_be32(p + 28);
    o->total_ticks    = mca_be64(p + 32);
    o->busy_ticks     = mca_be64(p + 40);
    o->adc_sat_events = mca_be32(p + 48);
    o->status_flags   = mca_be32(p + 52);
    return 0;
}

int mca_parse_mcrs(const uint8_t *p, size_t len, mca_mcrs_t *o)
{
    if (mca_pkt_type(p, len) != MCA_PKT_MCRS) return -1;
    o->cmd    = p[4];
    o->seq    = p[5];
    o->status = p[6];
    o->value  = mca_be32(p + 8);
    return 0;
}

int mca_parse_mcwf(const uint8_t *p, size_t len, mca_mcwf_t *o)
{
    if (mca_pkt_type(p, len) != MCA_PKT_MCWF) return -1;
    o->src        = p[5];
    o->n          = mca_be16(p + 6);
    o->capture_id = mca_be32(p + 8);
    o->offset     = mca_be16(p + 12);
    o->total      = mca_be16(p + 14);
    o->trig_ts    = mca_be32(p + 16);
    o->pretrig    = mca_be16(p + 20);
    o->samples    = p + 24;
    return 0;
}

int16_t mca_mcwf_sample(const mca_mcwf_t *w, unsigned i)
{
    return (int16_t)mca_be16(w->samples + 2u * i);
}

size_t mca_build_mccm(uint8_t *b, uint8_t cmd, uint8_t seq, uint32_t addr, uint32_t value)
{
    memcpy(b, "MCCM", 4);
    b[4] = cmd;
    b[5] = seq;
    b[6] = 0;
    b[7] = 0;
    mca_put_be32(b + 8, addr);
    mca_put_be32(b + 12, value);
    return 16;
}

const char *mca_cmd_name(uint8_t cmd)
{
    static const char *n[] = {"?", "PING", "WRITE_REG", "READ_REG", "CLEAR_COUNTERS",
                              "SET_STREAM", "CAPTURE_WAVE", "GET_STATUS", "RESET", "SET_NET"};
    return cmd < sizeof(n) / sizeof(n[0]) ? n[cmd] : "?";
}

const char *mca_status_name(uint8_t st)
{
    switch (st) {
    case MCA_ST_OK:        return "OK";
    case MCA_ST_BAD_CMD:   return "BAD_CMD";
    case MCA_ST_BAD_ADDR:  return "BAD_ADDR";
    case MCA_ST_BAD_VALUE: return "BAD_VALUE";
    case MCA_ST_BUSY:      return "BUSY";
    case MCA_ST_TIMEOUT:   return "TIMEOUT";
    default:               return "?";
    }
}

uint32_t mca_crc32(uint32_t crc, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = ~crc;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}
