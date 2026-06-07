/*
 * rtc_listener_internal.h - Internal listener demux registration hooks.
 */
#ifndef RTC_LISTENER_INTERNAL_H
#define RTC_LISTENER_INTERNAL_H

#include "rtc/rtc_listener.h"

#include "rtc_packet_io.h"
#include "rtc_stun.h"

typedef void (*rtc_listener_packet_fn)(rtc_pkt_type_t type, const uint8_t *data, size_t len,
                                       const rtc_addr_t *from, void *user);

int rtc_listener_register_ufrag(rtc_listener_t *listener, const char *ufrag,
                                rtc_listener_packet_fn fn, void *user);
void rtc_listener_unregister_ufrag(rtc_listener_t *listener, const char *ufrag);

int rtc_listener_register_stun_txn(rtc_listener_t *listener,
                                   const uint8_t txn_id[STUN_TXN_ID_SIZE],
                                   rtc_listener_packet_fn fn, void *user);
void rtc_listener_unregister_stun_txn(rtc_listener_t *listener,
                                      const uint8_t txn_id[STUN_TXN_ID_SIZE]);

int rtc_listener_register_tuple(rtc_listener_t *listener, const rtc_addr_t *remote,
                                rtc_listener_packet_fn fn, void *user);
void rtc_listener_unregister_tuple(rtc_listener_t *listener, const rtc_addr_t *remote);

int rtc_listener_send_to(rtc_listener_t *listener, const uint8_t *data, size_t len,
                         const rtc_addr_t *dest);

#endif /* RTC_LISTENER_INTERNAL_H */