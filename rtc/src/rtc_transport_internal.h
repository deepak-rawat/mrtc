/*
 * rtc_transport_internal.h - Internal logical transport constructors.
 */
#ifndef RTC_TRANSPORT_INTERNAL_H
#define RTC_TRANSPORT_INTERNAL_H

#include "rtc/rtc_router.h"

rtc_transport_t *rtc_transport_create_internal(rtc_router_t *router,
                                               const rtc_transport_config_t *cfg);

#endif /* RTC_TRANSPORT_INTERNAL_H */