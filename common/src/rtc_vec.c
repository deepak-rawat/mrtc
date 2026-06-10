/*
 * Generic dynamic array implementation.
 */
#include "rtc/rtc_vec.h"

#include <stdlib.h>
#include <string.h>

#define RTC_VEC_INITIAL_CAP 8

rtc_err_t rtc_vec_init(rtc_vec_t *v, size_t elem_size) {
    if (!v || elem_size == 0)
        return RTC_ERR_INVALID;
    v->data = NULL;
    v->len = 0;
    v->cap = 0;
    v->elem_size = elem_size;
    return RTC_OK;
}

rtc_err_t rtc_vec_reserve(rtc_vec_t *v, size_t min_cap) {
    if (!v || v->elem_size == 0)
        return RTC_ERR_INVALID;
    if (min_cap <= v->cap)
        return RTC_OK;

    size_t new_cap = v->cap ? v->cap : RTC_VEC_INITIAL_CAP;
    while (new_cap < min_cap) {
        /* Guard against multiplicative overflow */
        if (new_cap > (SIZE_MAX / 2))
            return RTC_ERR_NOMEM;
        new_cap *= 2;
    }

    /* Guard against byte-count overflow */
    if (new_cap > SIZE_MAX / v->elem_size)
        return RTC_ERR_NOMEM;

    void *new_data = realloc(v->data, new_cap * v->elem_size);
    if (!new_data)
        return RTC_ERR_NOMEM;

    v->data = new_data;
    v->cap = new_cap;
    return RTC_OK;
}

rtc_err_t rtc_vec_push(rtc_vec_t *v, const void *elem) {
    if (!v || !elem || v->elem_size == 0)
        return RTC_ERR_INVALID;

    if (v->len >= v->cap) {
        rtc_err_t rc = rtc_vec_reserve(v, v->len + 1);
        if (rc != RTC_OK)
            return rc;
    }

    memcpy((char *)v->data + v->len * v->elem_size, elem, v->elem_size);
    v->len++;
    return RTC_OK;
}

void *rtc_vec_at(const rtc_vec_t *v, size_t idx) {
    if (!v || !v->data || idx >= v->len)
        return NULL;
    return (char *)v->data + idx * v->elem_size;
}

size_t rtc_vec_len(const rtc_vec_t *v) {
    return v ? v->len : 0;
}

void rtc_vec_remove(rtc_vec_t *v, size_t idx) {
    if (!v || idx >= v->len)
        return;
    size_t tail = v->len - idx - 1;
    if (tail > 0) {
        char *base = (char *)v->data + idx * v->elem_size;
        memmove(base, base + v->elem_size, tail * v->elem_size);
    }
    v->len--;
}

void rtc_vec_swap_remove(rtc_vec_t *v, size_t idx) {
    if (!v || idx >= v->len)
        return;
    size_t last = v->len - 1;
    if (idx != last) {
        memcpy((char *)v->data + idx * v->elem_size, (char *)v->data + last * v->elem_size,
               v->elem_size);
    }
    v->len--;
}

void rtc_vec_clear(rtc_vec_t *v) {
    if (v)
        v->len = 0;
}

void rtc_vec_free(rtc_vec_t *v) {
    if (!v)
        return;
    free(v->data);
    v->data = NULL;
    v->len = 0;
    v->cap = 0;
    /* keep elem_size so the vector can be reused without re-init */
}
