/*
 * rtc_u32_map.c - Open-addressing hash map from uint32_t to void*.
 *
 * Implementation notes:
 *   - Linear probing on a power-of-2 sized table.
 *   - Fibonacci hashing: hash = key * 2654435769u, slot = hash >> shift.
 *   - Grows when (used + tombstones) > 3/4 * cap.
 *   - Rebuilds (without growing) when tombstones >= 1/8 * cap and load < 1/2.
 */
#include "rtc/rtc_u32_map.h"

#include <stdlib.h>
#include <string.h>

#define RTC_U32_MAP_INITIAL_CAP 16

#define ENTRY_EMPTY     0
#define ENTRY_USED      1
#define ENTRY_TOMBSTONE 2

/* Fibonacci multiplier (golden ratio * 2^32). */
#define FIB_MUL 2654435769u

static size_t u32_probe(uint32_t key, size_t cap) {
    /* cap is power of 2 -> mask = cap - 1 */
    return ((size_t)(key * FIB_MUL)) & (cap - 1);
}

static rtc_err_t u32_rehash(rtc_u32_map_t *m, size_t new_cap) {
    /* new_cap must be a power of 2 */
    rtc_u32_map_entry_t *new_entries =
        (rtc_u32_map_entry_t *)calloc(new_cap, sizeof(rtc_u32_map_entry_t));
    if (!new_entries)
        return RTC_ERR_NOMEM;

    size_t mask = new_cap - 1;

    /* Re-insert all used entries into the new table */
    if (m->entries) {
        for (size_t i = 0; i < m->cap; i++) {
            rtc_u32_map_entry_t *src = &m->entries[i];
            if (src->state != ENTRY_USED)
                continue;

            size_t slot = ((size_t)(src->key * FIB_MUL)) & mask;
            while (new_entries[slot].state == ENTRY_USED) {
                slot = (slot + 1) & mask;
            }
            new_entries[slot].key = src->key;
            new_entries[slot].value = src->value;
            new_entries[slot].state = ENTRY_USED;
        }
    }

    free(m->entries);
    m->entries = new_entries;
    m->cap = new_cap;
    m->tombstones = 0;
    return RTC_OK;
}

rtc_err_t rtc_u32_map_init(rtc_u32_map_t *m) {
    if (!m)
        return RTC_ERR_INVALID;
    m->entries = NULL;
    m->cap = 0;
    m->len = 0;
    m->tombstones = 0;
    return RTC_OK;
}

/* Find slot: returns index of matching slot, or first viable insertion slot
 * (empty or tombstone). 'found' tells which case. */
static size_t u32_find_slot(const rtc_u32_map_t *m, uint32_t key, bool *found) {
    size_t mask = m->cap - 1;
    size_t slot = u32_probe(key, m->cap);
    size_t first_tombstone = (size_t)-1;

    while (1) {
        rtc_u32_map_entry_t *e = &m->entries[slot];
        if (e->state == ENTRY_EMPTY) {
            *found = false;
            return (first_tombstone != (size_t)-1) ? first_tombstone : slot;
        }
        if (e->state == ENTRY_USED && e->key == key) {
            *found = true;
            return slot;
        }
        if (e->state == ENTRY_TOMBSTONE && first_tombstone == (size_t)-1) {
            first_tombstone = slot;
        }
        slot = (slot + 1) & mask;
    }
}

rtc_err_t rtc_u32_map_set(rtc_u32_map_t *m, uint32_t key, void *value) {
    if (!m)
        return RTC_ERR_INVALID;

    if (m->cap == 0) {
        rtc_err_t rc = u32_rehash(m, RTC_U32_MAP_INITIAL_CAP);
        if (rc != RTC_OK)
            return rc;
    } else if ((m->len + m->tombstones) * 4 >= m->cap * 3) {
        /* Load (used + tombstones) >= 75% - grow */
        if (m->cap > (SIZE_MAX / 2))
            return RTC_ERR_NOMEM;
        rtc_err_t rc = u32_rehash(m, m->cap * 2);
        if (rc != RTC_OK)
            return rc;
    }

    bool found;
    size_t slot = u32_find_slot(m, key, &found);
    rtc_u32_map_entry_t *e = &m->entries[slot];

    if (found) {
        e->value = value;
        return RTC_OK;
    }

    if (e->state == ENTRY_TOMBSTONE)
        m->tombstones--;
    e->key = key;
    e->value = value;
    e->state = ENTRY_USED;
    m->len++;
    return RTC_OK;
}

void *rtc_u32_map_get(const rtc_u32_map_t *m, uint32_t key) {
    if (!m || m->cap == 0)
        return NULL;
    bool found;
    size_t slot = u32_find_slot(m, key, &found);
    return found ? m->entries[slot].value : NULL;
}

bool rtc_u32_map_has(const rtc_u32_map_t *m, uint32_t key) {
    if (!m || m->cap == 0)
        return false;
    bool found;
    (void)u32_find_slot(m, key, &found);
    return found;
}

bool rtc_u32_map_remove(rtc_u32_map_t *m, uint32_t key) {
    if (!m || m->cap == 0)
        return false;
    bool found;
    size_t slot = u32_find_slot(m, key, &found);
    if (!found)
        return false;
    m->entries[slot].state = ENTRY_TOMBSTONE;
    m->entries[slot].value = NULL;
    m->len--;
    m->tombstones++;

    /* If tombstones dominate and load is low, rebuild same-size to compact */
    if (m->tombstones * 8 >= m->cap && m->len * 2 < m->cap && m->cap > RTC_U32_MAP_INITIAL_CAP) {
        (void)u32_rehash(m, m->cap); /* best-effort; failure is harmless */
    }
    return true;
}

size_t rtc_u32_map_len(const rtc_u32_map_t *m) {
    return m ? m->len : 0;
}

void rtc_u32_map_clear(rtc_u32_map_t *m) {
    if (!m || !m->entries)
        return;
    memset(m->entries, 0, m->cap * sizeof(rtc_u32_map_entry_t));
    m->len = 0;
    m->tombstones = 0;
}

void rtc_u32_map_free(rtc_u32_map_t *m) {
    if (!m)
        return;
    free(m->entries);
    m->entries = NULL;
    m->cap = 0;
    m->len = 0;
    m->tombstones = 0;
}

bool rtc_u32_map_next(const rtc_u32_map_t *m, rtc_u32_map_iter_t *it, uint32_t *key, void **value) {
    if (!m || !it || m->cap == 0)
        return false;
    while (it->idx < m->cap) {
        rtc_u32_map_entry_t *e = &m->entries[it->idx++];
        if (e->state == ENTRY_USED) {
            if (key)
                *key = e->key;
            if (value)
                *value = e->value;
            return true;
        }
    }
    return false;
}
