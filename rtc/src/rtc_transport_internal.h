/*
 * Internal logical transport constructors.
 */
#ifndef RTC_TRANSPORT_INTERNAL_H
#define RTC_TRANSPORT_INTERNAL_H

#include "rtc/rtc_transport.h"
#include "rtc_rtp_pacer.h"

#ifdef MRTC_ENABLE_TWCC
#  include "rtc_twcc_sender.h"
#endif

int rtc_transport_send_raw(rtc_transport_t *transport, const uint8_t *data, size_t len);
rtc_worker_t *rtc_transport_worker(rtc_transport_t *transport);
const char *rtc_transport_cname(const rtc_transport_t *transport);
/* No actor pointers escape these calls. Timers belong to the transport and
 * close cancels them on its worker before destroy frees the transport. */
int rtc_transport_send_rtp_media(rtc_transport_t *transport, uint8_t *buf, size_t *len, size_t cap,
                                 bool video, uint32_t bitrate_bps);
void rtc_transport_discard_rtp(rtc_transport_t *transport, uint32_t ssrc);
void rtc_transport_forget_rtp(rtc_transport_t *transport, uint32_t ssrc);
bool rtc_transport_sync_rtp_stats(rtc_transport_t *transport, rtc_rtcp_stats_t *stats,
                                  uint64_t *dropped);
/* Retain only while holding an existing live reference. destroy closes and
 * releases the owner's reference; release never starts worker teardown. */
void rtc_transport_retain(rtc_transport_t *transport);
void rtc_transport_release(rtc_transport_t *transport);
bool rtc_transport_rtp_pacing_enabled(const rtc_transport_t *transport);
bool rtc_transport_rtp_sent(rtc_transport_t *transport, uint32_t ssrc, rtc_pacer_sent_t *out);

#ifdef MRTC_ENABLE_TWCC
/* TWCC sender history for outbound RTP tagging, or NULL when TWCC is not
 * enabled. Send streams pull this from their attached transport. */
rtc_twcc_sender_t *rtc_transport_twcc_sender(rtc_transport_t *transport);
#endif

#endif /* RTC_TRANSPORT_INTERNAL_H */
