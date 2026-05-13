/*
 * rtc_vec.h - Generic dynamic array.
 *
 * Byte-element typed array with O(1) amortized push, O(1) random access,
 * and explicit element size. Not thread-safe; caller serializes access.
 *
 * Pointer stability: any operation that may grow the vector
 * (rtc_vec_push, rtc_vec_reserve) can invalidate pointers returned by
 * rtc_vec_at(). Do not hold pointers across such calls.
 *
 * Usage:
 *     rtc_vec_t v;
 *     rtc_vec_init(&v, sizeof(int));
 *     int x = 42;
 *     rtc_vec_push(&v, &x);
 *     int *p = (int *)rtc_vec_at(&v, 0);
 *     rtc_vec_free(&v);
 */
#ifndef RTC_VEC_H
#define RTC_VEC_H

#include "rtc_common.h"

typedef struct {
    void *data;       /* raw buffer (len * elem_size bytes) */
    size_t len;       /* number of elements in use */
    size_t cap;       /* capacity in elements */
    size_t elem_size; /* bytes per element */
} rtc_vec_t;

/* Initialize an empty vector. elem_size must be > 0. */
rtc_err_t rtc_vec_init(rtc_vec_t *v, size_t elem_size);

/* Reserve capacity for at least 'min_cap' elements. Existing data preserved. */
rtc_err_t rtc_vec_reserve(rtc_vec_t *v, size_t min_cap);

/* Append elem_size bytes from 'elem' to the end of the vector.
 * Grows storage if needed. */
rtc_err_t rtc_vec_push(rtc_vec_t *v, const void *elem);

/* Pointer to element at index (0-based). Returns NULL if out of range.
 * Pointer is invalidated by any operation that may grow the vector. */
void *rtc_vec_at(const rtc_vec_t *v, size_t idx);

/* Number of elements in the vector. */
size_t rtc_vec_len(const rtc_vec_t *v);

/* Remove element at idx, shifting later elements down. O(n). */
void rtc_vec_remove(rtc_vec_t *v, size_t idx);

/* Remove element at idx by overwriting it with the last element. O(1).
 * Does not preserve order. */
void rtc_vec_swap_remove(rtc_vec_t *v, size_t idx);

/* Set length to 0 but retain allocated capacity. */
void rtc_vec_clear(rtc_vec_t *v);

/* Free the backing buffer and reset the vector to empty. Safe on zero-init. */
void rtc_vec_free(rtc_vec_t *v);

#endif /* RTC_VEC_H */
