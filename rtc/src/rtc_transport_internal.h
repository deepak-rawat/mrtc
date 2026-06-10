/*
 * Internal logical transport constructors.
 */
#ifndef RTC_TRANSPORT_INTERNAL_H
#define RTC_TRANSPORT_INTERNAL_H

#include "rtc/rtc_transport.h"

int rtc_transport_send_raw(rtc_transport_t *transport, const uint8_t *data, size_t len);

#endif /* RTC_TRANSPORT_INTERNAL_H */
