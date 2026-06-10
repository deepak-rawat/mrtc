/*
 * Open-addressing hash map from uint32_t key to void* value.
 *
 * Designed for hot-path SSRC -> receiver lookups: O(1) amortized, no
 * allocations per get/set after rehash, cache-friendly linear probing,
 * power-of-2 capacity, Fibonacci hashing.
 *
 * Not thread-safe. Caller serializes access.
 *
 * Pointer stability: pointers returned by rtc_u32_map_get() are NOT
 * stable across set/remove operations that may trigger a rehash.
 *
 * Usage:
 *     rtc_u32_map_t m;
 *     rtc_u32_map_init(&m);
 *     rtc_u32_map_set(&m, 0xCAFEBABE, my_obj);
 *     void *p = rtc_u32_map_get(&m, 0xCAFEBABE);
 *     rtc_u32_map_free(&m);
 */
#ifndef RTC_U32_MAP_H
#define RTC_U32_MAP_H

#include "rtc_common.h"

typedef struct {
    uint32_t key;
    uint8_t state; /* 0 = empty, 1 = used, 2 = tombstone */
    void *value;
} rtc_u32_map_entry_t;

typedef struct {
    rtc_u32_map_entry_t *entries;
    size_t cap; /* power of 2; 0 if empty */
    size_t len; /* number of used (non-tombstone) entries */
    size_t tombstones;
} rtc_u32_map_t;

typedef struct {
    size_t idx; /* internal cursor; caller must zero-init */
} rtc_u32_map_iter_t;

/* Initialize an empty map. */
rtc_err_t rtc_u32_map_init(rtc_u32_map_t *m);

/* Insert or overwrite key=value. */
rtc_err_t rtc_u32_map_set(rtc_u32_map_t *m, uint32_t key, void *value);

/* Lookup. Returns the stored value or NULL if key is absent.
 * NOTE: NULL is a valid stored value, so use rtc_u32_map_has() to
 * disambiguate if necessary. */
void *rtc_u32_map_get(const rtc_u32_map_t *m, uint32_t key);

/* Returns true if key is present. */
bool rtc_u32_map_has(const rtc_u32_map_t *m, uint32_t key);

/* Remove an entry. Returns true if key was present. */
bool rtc_u32_map_remove(rtc_u32_map_t *m, uint32_t key);

/* Number of used entries. */
size_t rtc_u32_map_len(const rtc_u32_map_t *m);

/* Empty all entries but keep allocated capacity. */
void rtc_u32_map_clear(rtc_u32_map_t *m);

/* Free the backing storage. Safe on zero-init. */
void rtc_u32_map_free(rtc_u32_map_t *m);

/* Iterate over all (key, value) pairs in unspecified order.
 * Returns true and fills key/value (when non-NULL) while entries remain;
 * returns false at end. Must not modify the map during iteration. */
bool rtc_u32_map_next(const rtc_u32_map_t *m, rtc_u32_map_iter_t *it, uint32_t *key, void **value);

#endif /* RTC_U32_MAP_H */
