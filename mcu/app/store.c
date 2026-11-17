#include "store.h"

#include <string.h>

#include "acq.h"
#include "platform.h"
#include "settings.h"
#include "spectrum.h"

typedef char hdr_fits[sizeof(store_hdr_t) <= STORE_HDR_SIZE ? 1 : -1];

unsigned store_slots(void)
{
    unsigned n = plat_nv_slots();
    return n > 1 ? n - 1 : 0;
}

static int slot_ok(unsigned slot, uint32_t nch)
{
    return slot >= 1 && slot <= store_slots() && STORE_HDR_SIZE + (size_t)nch * 4u <= plat_nv_slot_size();
}

static uint32_t hdr_crc(store_hdr_t h)
{
    h.crc = 0;
    return mca_crc32(0, &h, sizeof(h));
}

int store_save(unsigned slot, const char *name)
{
    if (!slot_ok(slot, g_spec.nch)) return -1;
    uint8_t buf[STORE_HDR_SIZE];
    store_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.magic = STORE_MAGIC;
    h.version = STORE_VERSION;
    h.nch = g_spec.nch;
    h.shift = g_spec.shift;
    h.total = g_spec.total;
    h.overflow = g_spec.overflow;
    h.st = g_acq.st;
    h.counts_at_st = g_acq.counts_at_st;
    h.events_at_st = g_acq.events_at_st;
    memcpy(h.cal, g_set.cal, sizeof(h.cal));
    memcpy(h.unit, g_set.cal_unit, sizeof(h.unit));
    if (name) strncpy(h.name, name, sizeof(h.name) - 1);
    h.crc = mca_crc32(hdr_crc(h), g_spec.h, (size_t)h.nch * 4u);

    memset(buf, 0xFF, sizeof(buf));
    memcpy(buf, &h, sizeof(h));
    if (plat_nv_erase(slot)) return -1;
    if (plat_nv_write(slot, 0, buf, sizeof(buf))) return -1;
    return plat_nv_write(slot, STORE_HDR_SIZE, g_spec.h, (size_t)h.nch * 4u);
}

int store_info(unsigned slot, store_hdr_t *h)
{
    if (slot < 1 || slot > store_slots()) return -1;
    if (plat_nv_read(slot, 0, h, sizeof(*h))) return -1;
    if (h->magic != STORE_MAGIC || h->version != STORE_VERSION || !slot_ok(slot, h->nch) || h->nch > SPEC_MAX_CH) return -1;
    return 0;
}

int store_load(unsigned slot)
{
    store_hdr_t h;
    if (store_info(slot, &h)) return -1;
    /* 先读到 g_spec.h 再校验；失败时清空，避免显示半截数据 */
    spec_reset(h.nch, h.shift);
    if (plat_nv_read(slot, STORE_HDR_SIZE, g_spec.h, (size_t)h.nch * 4u) ||
        mca_crc32(hdr_crc(h), g_spec.h, (size_t)h.nch * 4u) != h.crc) {
        spec_reset(g_set.nch, settings_shift());
        return -1;
    }
    g_spec.total = h.total;
    g_spec.overflow = h.overflow;
    acq_clear();
    g_acq.st = h.st;
    g_acq.have_st = 1;
    g_acq.counts_at_st = h.counts_at_st;
    g_acq.events_at_st = h.events_at_st;
    g_acq.events = h.events_at_st;
    g_acq.frozen = 1;
    memcpy(g_set.cal, h.cal, sizeof(h.cal));
    memcpy(g_set.cal_unit, h.unit, sizeof(h.unit));
    g_set.cal_unit[sizeof(g_set.cal_unit) - 1] = 0;
    spec_analyze();
    return 0;
}
