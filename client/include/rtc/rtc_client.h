/*
 * Client API umbrella.
 *
 */
#ifndef RTC_CLIENT_H
#define RTC_CLIENT_H

#include <rtc/rtc.h>
#include "rtc_track.h"
#include "rtc_data_channel.h"
#include "rtc_peer.h"
#include "rtc_stats.h"

/*
 * Library lifecycle for client consumers (call once per process).
 *
 * rtc_client_init() calls rtc_init() and additionally brings up the
 * shared peer-connection runtime (worker + UDP listener + router).
 * rtc_client_cleanup() tears that runtime down and then calls
 * rtc_cleanup(). Apps that link `libmrtc_client` should call this
 * pair instead of rtc_init()/rtc_cleanup() directly.
 */
int rtc_client_init(void);
void rtc_client_cleanup(void);

#endif /* RTC_CLIENT_H */
