#include "settings.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mca_proto.h"
#include "platform.h"

settings_t g_set;

#define IP4(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

void settings_defaults(settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->magic   = SETTINGS_MAGIC;
    s->version = SETTINGS_VERSION;
    s->fpga_ip = IP4(192, 168, 1, 10);
    s->mcu_ip  = IP4(192, 168, 1, 20);
    s->netmask = IP4(255, 255, 255, 0);
    s->gateway = IP4(192, 168, 1, 1);
    s->tau_us  = 32.0f;            /* 与 FPGA 测试源 decay=10（1024 采样）一致 */
    s->rise_us = 2.0f;
    s->flat_us = 1.0f;
    s->fast_us = 0.25f;
    s->gap_us  = 0.25f;
    s->thr_lsb = 8.0f;
    s->test_src = 1;               /* 没接探测器时先用测试源跑通 */
    s->tp_rate = 5000.0f;
    s->tp_amp  = 100.0f;
    s->nch       = 4096;
    s->log_scale = 1;
    s->stream_n  = 64;
    s->stream_ms = 1;
    s->cal[1] = 1.0f;
    strcpy(s->cal_unit, "ch");
}

uint32_t settings_crc(const settings_t *s)
{
    return mca_crc32(0, s, offsetof(settings_t, crc));    /* 不含末尾 crc 字段 */
}

int settings_load(void)
{
    settings_t t;
    if (plat_nv_read(0, 0, &t, sizeof(t)) == 0 && t.magic == SETTINGS_MAGIC &&
        t.version == SETTINGS_VERSION && t.crc == settings_crc(&t)) {
        g_set = t;
        return 0;
    }
    settings_defaults(&g_set);
    return -1;
}

int settings_save(void)
{
    g_set.magic   = SETTINGS_MAGIC;
    g_set.version = SETTINGS_VERSION;
    g_set.crc     = settings_crc(&g_set);
    if (plat_nv_erase(0)) return -1;
    return plat_nv_write(0, 0, &g_set, sizeof(g_set));
}

unsigned settings_shift(void)
{
    unsigned s = 0;
    while ((65536u >> s) > g_set.nch && s < 8) s++;
    return s;
}

/* ---------------- 文本接口 ---------------- */
typedef enum { T_IP, T_F32, T_U8, T_U16, T_STR } kind_t;
typedef struct {
    const char *key;
    kind_t kind;
    size_t off;
    float lo, hi;
} field_t;

#define F(k, t, m, lo, hi) {k, t, offsetof(settings_t, m), lo, hi}
static const field_t fields[] = {
    F("fpga_ip", T_IP, fpga_ip, 0, 0),
    F("mcu_ip", T_IP, mcu_ip, 0, 0),
    F("netmask", T_IP, netmask, 0, 0),
    F("gateway", T_IP, gateway, 0, 0),
    F("tau_us", T_F32, tau_us, 0.05f, 100000.0f),
    F("rise_us", T_F32, rise_us, 0.05f, 31.9f),
    F("flat_us", T_F32, flat_us, 0.0f, 7.9f),
    F("fast_us", T_F32, fast_us, 0.03f, 1.9f),
    F("gap_us", T_F32, gap_us, 0.0f, 1.9f),
    F("thr_lsb", T_F32, thr_lsb, 0.2f, 255.0f),
    F("polarity_neg", T_U8, polarity_neg, 0, 1),
    F("pu_discard", T_U8, pu_discard, 0, 1),
    F("test_src", T_U8, test_src, 0, 1),
    F("keep_pileup", T_U8, keep_pileup, 0, 1),
    F("tp_rate", T_F32, tp_rate, 0.0f, 2.0e6f),
    F("tp_amp", T_F32, tp_amp, 0.0f, 255.0f),
    F("nch", T_U16, nch, 256, 8192),
    F("log_scale", T_U8, log_scale, 0, 1),
    F("stream_n", T_U8, stream_n, 1, 64),
    F("stream_ms", T_U16, stream_ms, 1, 10000),
    F("cal0", T_F32, cal[0], -1e9f, 1e9f),
    F("cal1", T_F32, cal[1], -1e9f, 1e9f),
    F("cal2", T_F32, cal[2], -1e9f, 1e9f),
    F("cal_unit", T_STR, cal_unit, 0, 0),
};

static int parse_ip(const char *s, uint32_t *ip)
{
    unsigned a, b, c, d;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return -1;
    *ip = IP4(a, b, c, d);
    return 0;
}

int settings_set(const char *key, const char *value)
{
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        const field_t *f = &fields[i];
        if (strcmp(f->key, key) != 0) continue;
        uint8_t *p = (uint8_t *)&g_set + f->off;
        char *end;
        float v = 0;
        if (f->kind == T_IP) return parse_ip(value, (uint32_t *)p);
        if (f->kind == T_STR) {
            strncpy((char *)p, value, sizeof(g_set.cal_unit) - 1);
            ((char *)p)[sizeof(g_set.cal_unit) - 1] = 0;
            return 0;
        }
        v = strtof(value, &end);
        if (end == value || v < f->lo || v > f->hi) return -1;
        if (f->kind == T_F32) *(float *)p = v;
        else if (f->kind == T_U8) *p = (uint8_t)v;
        else {
            uint16_t u = (uint16_t)v;
            if (strcmp(key, "nch") == 0 && (u & (u - 1u))) return -1;   /* 道数须为 2 的幂 */
            memcpy(p, &u, 2);
        }
        return 0;
    }
    return -1;
}

static int fmt_field(const field_t *f, char *out, int n)
{
    const uint8_t *p = (const uint8_t *)&g_set + f->off;
    switch (f->kind) {
    case T_IP: {
        uint32_t ip;
        memcpy(&ip, p, 4);
        return snprintf(out, n, "%u.%u.%u.%u", (unsigned)(ip >> 24), (unsigned)(ip >> 16) & 255,
                        (unsigned)(ip >> 8) & 255, (unsigned)ip & 255);
    }
    case T_F32: {
        float v;
        memcpy(&v, p, 4);
        return snprintf(out, n, "%g", (double)v);
    }
    case T_U8:  return snprintf(out, n, "%u", (unsigned)*p);
    case T_U16: {
        uint16_t v;
        memcpy(&v, p, 2);
        return snprintf(out, n, "%u", (unsigned)v);
    }
    default:    return snprintf(out, n, "%s", (const char *)p);
    }
}

int settings_get(const char *key, char *out, int n)
{
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
        if (strcmp(fields[i].key, key) == 0) return fmt_field(&fields[i], out, n) >= 0 ? 0 : -1;
    return -1;
}

int settings_dump(char *out, int n)
{
    int k = 0;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]) && k < n; i++) {
        k += snprintf(out + k, n - k, "%s=", fields[i].key);
        if (k >= n) break;
        k += fmt_field(&fields[i], out + k, n - k);
        if (k < n) out[k++] = '\n';
    }
    if (k >= n) k = n - 1;
    out[k] = 0;
    return k;
}
