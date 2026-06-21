/*
 * RTCP interceptor chain. See rtc_interceptor.h.
 */
#include "rtc/rtc_interceptor.h"

void rtc_interceptor_chain_init(rtc_interceptor_chain_t *chain) {
    if (chain)
        chain->count = 0;
}

rtc_err_t rtc_interceptor_chain_add(rtc_interceptor_chain_t *chain, rtc_interceptor_t *it) {
    if (!chain || !it || !it->ops)
        return RTC_ERR_INVALID;
    if (chain->count >= RTC_INTERCEPTOR_CHAIN_MAX)
        return RTC_ERR_NOMEM;
    chain->items[chain->count++] = it;
    return RTC_OK;
}

void rtc_interceptor_chain_on_rtcp(rtc_interceptor_chain_t *chain, uint8_t pt, uint8_t fmt,
                                   const uint8_t *buf, size_t len) {
    if (!chain)
        return;
    for (int i = 0; i < chain->count; i++) {
        rtc_interceptor_t *it = chain->items[i];
        if (it->ops->on_rtcp)
            it->ops->on_rtcp(it, pt, fmt, buf, len);
    }
}

void rtc_interceptor_chain_tick(rtc_interceptor_chain_t *chain, uint64_t now_ms) {
    if (!chain)
        return;
    for (int i = 0; i < chain->count; i++) {
        rtc_interceptor_t *it = chain->items[i];
        if (it->ops->on_tick)
            it->ops->on_tick(it, now_ms);
    }
}

void rtc_interceptor_chain_close(rtc_interceptor_chain_t *chain) {
    if (!chain)
        return;
    for (int i = 0; i < chain->count; i++) {
        rtc_interceptor_t *it = chain->items[i];
        if (it->ops->destroy)
            it->ops->destroy(it);
        chain->items[i] = NULL;
    }
    chain->count = 0;
}
