/*
 * Internal logical transport constructors.
 */
#ifndef RTC_TRANSPORT_INTERNAL_H
#define RTC_TRANSPORT_INTERNAL_H

#include "rtc/rtc_transport.h"

#ifdef MRTC_ENABLE_TWCC
#  include "rtc_twcc_sender.h"
#endif

int rtc_transport_send_raw(rtc_transport_t *transport, const uint8_t *data, size_t len);

#ifdef MRTC_ENABLE_TWCC
/* TWCC sender history for outbound RTP tagging, or NULL when TWCC is not
 * enabled. Send streams pull this from their attached transport. */
rtc_twcc_sender_t *rtc_transport_twcc_sender(rtc_transport_t *transport);
#endif

#endif /* RTC_TRANSPORT_INTERNAL_H */
