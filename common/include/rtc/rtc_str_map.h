/*
 * Open-addressing hash map from string key to void* value.
 *
 * Used for peer_id, meeting name, and label-based lookups across mrtc.
 * O(1) amortized, linear probing on power-of-2 sized table, FNV-1a hashing.
 *
 * Two key-ownership modes:
 *   - Borrowed: caller's string pointer is stored; caller must keep the
 *     string alive for the lifetime of the entry. Use rtc_str_map_init().
 *   - Owned:    key is copied (strdup) on insert and freed on remove/free.
 *     Use rtc_str_map_init_owned().
 *
 * Not thread-safe. Caller serializes access.
 *
 * Usage:
 *     rtc_str_map_t m;
 *     rtc_str_map_init_owned(&m);
 *     rtc_str_map_set(&m, "alice", peer_obj);
 *     void *p = rtc_str_map_get(&m, "alice");
 *     rtc_str_map_free(&m);
 */
#ifndef RTC_STR_MAP_H
#define RTC_STR_MAP_H

#include "rtc_common.h"

typedef struct {
    char *key; /* borrowed or strdup'd pointer; NULL if entry not used */
    void *value;
    uint32_t hash; /* cached FNV-1a hash of key */
    uint8_t state; /* 0 = empty, 1 = used, 2 = tombstone */
} rtc_str_map_entry_t;

typedef struct {
    rtc_str_map_entry_t *entries;
    size_t cap; /* power of 2; 0 if empty */
    size_t len;
    size_t tombstones;
    bool owns_keys;
} rtc_str_map_t;

typedef struct {
    size_t idx;
} rtc_str_map_iter_t;

/* Initialize map with borrowed string keys (caller owns key memory). */
rtc_err_t rtc_str_map_init(rtc_str_map_t *m);

/* Initialize map that strdup's keys on insert and frees them on remove/free. */
rtc_err_t rtc_str_map_init_owned(rtc_str_map_t *m);

/* Insert or overwrite key=value. In owned mode, 'key' is duplicated; in
 * borrowed mode, the pointer is stored as-is and must outlive the entry. */
rtc_err_t rtc_str_map_set(rtc_str_map_t *m, const char *key, void *value);

/* Returns value or NULL if absent. NULL is a valid stored value -- use
 * rtc_str_map_has() to disambiguate. */
void *rtc_str_map_get(const rtc_str_map_t *m, const char *key);

/* True iff key is present. */
bool rtc_str_map_has(const rtc_str_map_t *m, const char *key);

/* Remove an entry. Returns true if key was present. */
bool rtc_str_map_remove(rtc_str_map_t *m, const char *key);

/* Number of used entries. */
size_t rtc_str_map_len(const rtc_str_map_t *m);

/* Empty all entries (freeing owned keys) but keep allocated capacity. */
void rtc_str_map_clear(rtc_str_map_t *m);

/* Free storage (and owned keys, if applicable). Safe on zero-init. */
void rtc_str_map_free(rtc_str_map_t *m);

/* Iterate over entries in unspecified order. Returned key pointer aliases
 * map-internal storage; do not free or modify. */
bool rtc_str_map_next(const rtc_str_map_t *m, rtc_str_map_iter_t *it, const char **key,
                      void **value);

#endif /* RTC_STR_MAP_H */
