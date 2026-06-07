/*
 * SFU API umbrella.
 *
 * This API is enabled with MRTC_ENABLE_SFU_API. It will expose the server-side
 * RTC building blocks: worker, listener, transport, router, producer, and
 * consumer. The first implementation commits establish the public boundary;
 * concrete types are added as the runtime layers land.
 */
#ifndef RTC_SFU_H
#define RTC_SFU_H

#ifndef MRTC_ENABLE_SFU_API
#  error "rtc_sfu.h requires MRTC_ENABLE_SFU_API"
#endif

#include "rtc_worker.h"
#include "rtc_listener.h"
#include "rtc_transport.h"
#include "rtc_router.h"
#include "rtc_producer.h"
#include "rtc_consumer.h"
#include "rtc_rtp_params.h"

#endif /* RTC_SFU_H */