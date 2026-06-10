/*
 * Open-addressing hash map keyed by C strings.
 *
 * Implementation notes:
 *   - FNV-1a 32-bit hash of NUL-terminated key.
 *   - Linear probing on power-of-2 sized table.
 *   - Grows when (used + tombstones) > 3/4 * cap.
 *   - Cached hash stored per entry avoids re-hashing during probing/rehash.
 */
#include "rtc/rtc_str_map.h"

#include <stdlib.h>
#include <string.h>

#define RTC_STR_MAP_INITIAL_CAP 16

#define ENTRY_EMPTY     0
#define ENTRY_USED      1
#define ENTRY_TOMBSTONE 2

/* FNV-1a 32-bit. Avoid returning 0 since we never need a "no hash" sentinel,
 * but a stable hash for empty string is fine. */
static uint32_t fnv1a(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

static char *str_dup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

static rtc_err_t str_rehash(rtc_str_map_t *m, size_t new_cap) {
    rtc_str_map_entry_t *new_entries =
        (rtc_str_map_entry_t *)calloc(new_cap, sizeof(rtc_str_map_entry_t));
    if (!new_entries)
        return RTC_ERR_NOMEM;

    size_t mask = new_cap - 1;

    if (m->entries) {
        for (size_t i = 0; i < m->cap; i++) {
            rtc_str_map_entry_t *src = &m->entries[i];
            if (src->state != ENTRY_USED)
                continue;

            size_t slot = (size_t)src->hash & mask;
            while (new_entries[slot].state == ENTRY_USED) {
                slot = (slot + 1) & mask;
            }
            new_entries[slot] = *src;
        }
    }

    free(m->entries);
    m->entries = new_entries;
    m->cap = new_cap;
    m->tombstones = 0;
    return RTC_OK;
}

static rtc_err_t init_common(rtc_str_map_t *m, bool owns_keys) {
    if (!m)
        return RTC_ERR_INVALID;
    m->entries = NULL;
    m->cap = 0;
    m->len = 0;
    m->tombstones = 0;
    m->owns_keys = owns_keys;
    return RTC_OK;
}

rtc_err_t rtc_str_map_init(rtc_str_map_t *m) {
    return init_common(m, false);
}

rtc_err_t rtc_str_map_init_owned(rtc_str_map_t *m) {
    return init_common(m, true);
}

/* Find slot for key. Sets *found and returns slot index (for insert or match). */
static size_t str_find_slot(const rtc_str_map_t *m, const char *key, uint32_t hash, bool *found) {
    size_t mask = m->cap - 1;
    size_t slot = (size_t)hash & mask;
    size_t first_tombstone = (size_t)-1;

    while (1) {
        rtc_str_map_entry_t *e = &m->entries[slot];
        if (e->state == ENTRY_EMPTY) {
            *found = false;
            return (first_tombstone != (size_t)-1) ? first_tombstone : slot;
        }
        if (e->state == ENTRY_USED && e->hash == hash && strcmp(e->key, key) == 0) {
            *found = true;
            return slot;
        }
        if (e->state == ENTRY_TOMBSTONE && first_tombstone == (size_t)-1) {
            first_tombstone = slot;
        }
        slot = (slot + 1) & mask;
    }
}

rtc_err_t rtc_str_map_set(rtc_str_map_t *m, const char *key, void *value) {
    if (!m || !key)
        return RTC_ERR_INVALID;

    if (m->cap == 0) {
        rtc_err_t rc = str_rehash(m, RTC_STR_MAP_INITIAL_CAP);
        if (rc != RTC_OK)
            return rc;
    } else if ((m->len + m->tombstones) * 4 >= m->cap * 3) {
        if (m->cap > (SIZE_MAX / 2))
            return RTC_ERR_NOMEM;
        rtc_err_t rc = str_rehash(m, m->cap * 2);
        if (rc != RTC_OK)
            return rc;
    }

    uint32_t h = fnv1a(key);
    bool found;
    size_t slot = str_find_slot(m, key, h, &found);
    rtc_str_map_entry_t *e = &m->entries[slot];

    if (found) {
        e->value = value;
        return RTC_OK;
    }

    char *stored_key;
    if (m->owns_keys) {
        stored_key = str_dup(key);
        if (!stored_key)
            return RTC_ERR_NOMEM;
    } else {
        stored_key = (char *)key; /* borrowed */
    }

    if (e->state == ENTRY_TOMBSTONE)
        m->tombstones--;
    e->key = stored_key;
    e->value = value;
    e->hash = h;
    e->state = ENTRY_USED;
    m->len++;
    return RTC_OK;
}

void *rtc_str_map_get(const rtc_str_map_t *m, const char *key) {
    if (!m || !key || m->cap == 0)
        return NULL;
    bool found;
    size_t slot = str_find_slot(m, key, fnv1a(key), &found);
    return found ? m->entries[slot].value : NULL;
}

bool rtc_str_map_has(const rtc_str_map_t *m, const char *key) {
    if (!m || !key || m->cap == 0)
        return false;
    bool found;
    (void)str_find_slot(m, key, fnv1a(key), &found);
    return found;
}

bool rtc_str_map_remove(rtc_str_map_t *m, const char *key) {
    if (!m || !key || m->cap == 0)
        return false;
    bool found;
    size_t slot = str_find_slot(m, key, fnv1a(key), &found);
    if (!found)
        return false;

    rtc_str_map_entry_t *e = &m->entries[slot];
    if (m->owns_keys)
        free(e->key);
    e->key = NULL;
    e->value = NULL;
    e->hash = 0;
    e->state = ENTRY_TOMBSTONE;
    m->len--;
    m->tombstones++;

    if (m->tombstones * 8 >= m->cap && m->len * 2 < m->cap && m->cap > RTC_STR_MAP_INITIAL_CAP) {
        (void)str_rehash(m, m->cap);
    }
    return true;
}

size_t rtc_str_map_len(const rtc_str_map_t *m) {
    return m ? m->len : 0;
}

void rtc_str_map_clear(rtc_str_map_t *m) {
    if (!m || !m->entries)
        return;
    if (m->owns_keys) {
        for (size_t i = 0; i < m->cap; i++) {
            if (m->entries[i].state == ENTRY_USED)
                free(m->entries[i].key);
        }
    }
    memset(m->entries, 0, m->cap * sizeof(rtc_str_map_entry_t));
    m->len = 0;
    m->tombstones = 0;
}

void rtc_str_map_free(rtc_str_map_t *m) {
    if (!m)
        return;
    if (m->owns_keys && m->entries) {
        for (size_t i = 0; i < m->cap; i++) {
            if (m->entries[i].state == ENTRY_USED)
                free(m->entries[i].key);
        }
    }
    free(m->entries);
    m->entries = NULL;
    m->cap = 0;
    m->len = 0;
    m->tombstones = 0;
}

bool rtc_str_map_next(const rtc_str_map_t *m, rtc_str_map_iter_t *it, const char **key,
                      void **value) {
    if (!m || !it || m->cap == 0)
        return false;
    while (it->idx < m->cap) {
        rtc_str_map_entry_t *e = &m->entries[it->idx++];
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
