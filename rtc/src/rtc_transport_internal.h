/*
 * rtc_transport_internal.h - Internal logical transport constructors.
 */
#ifndef RTC_TRANSPORT_INTERNAL_H
#define RTC_TRANSPORT_INTERNAL_H

#include "rtc/rtc_router.h"

#include "rtc_rtp.h"

typedef struct rtc_producer rtc_producer_t;
typedef void (*rtc_transport_rtp_fn)(const rtc_rtp_packet_t *pkt, void *user);
typedef void (*rtc_transport_rtcp_fn)(const uint8_t *data, size_t len, void *user);
typedef void (*rtc_transport_data_fn)(const uint8_t *data, size_t len, void *user);

rtc_transport_t *rtc_transport_create_internal(rtc_router_t *router,
                                               const rtc_transport_config_t *cfg);
int rtc_transport_set_dtls_role_internal(rtc_transport_t *transport,
                                         rtc_transport_dtls_role_t role);
void rtc_transport_on_rtp(rtc_transport_t *transport, rtc_transport_rtp_fn fn, void *user);
void rtc_transport_on_rtcp(rtc_transport_t *transport, rtc_transport_rtcp_fn fn, void *user);
void rtc_transport_on_data(rtc_transport_t *transport, rtc_transport_data_fn fn, void *user);
int rtc_transport_send_data(rtc_transport_t *transport, const uint8_t *data, size_t len);
int rtc_transport_register_producer(rtc_transport_t *transport, rtc_producer_t *producer,
                                    uint32_t ssrc);
void rtc_transport_unregister_producer(rtc_transport_t *transport, uint32_t ssrc);
int rtc_transport_send_rtp(rtc_transport_t *transport, uint8_t *buf, size_t *len, size_t buf_cap);

#endif /* RTC_TRANSPORT_INTERNAL_H */