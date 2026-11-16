#ifndef RTC_SCTP_H
#define RTC_SCTP_H

#include "rtc_data_channel.h"

/* RFC 8831 SCTP/DCEP endpoint. Call on the owning transport worker.
 * The application must call rtc_client_init()/rtc_client_cleanup(). */
typedef struct rtc_dc_manager rtc_sctp_t;
typedef int (*rtc_sctp_send_fn)(const uint8_t *packet, size_t len, void *user);
typedef void (*rtc_sctp_on_channel_fn)(rtc_data_channel_t *channel, void *user);

rtc_sctp_t *rtc_sctp_create(rtc_sctp_send_fn send, void *user);
void rtc_sctp_destroy(rtc_sctp_t *sctp);
int rtc_sctp_connect(rtc_sctp_t *sctp, bool local_dtls_client);
int rtc_sctp_receive(rtc_sctp_t *sctp, const uint8_t *packet, size_t len);
/* Call periodically (e.g. every 10 ms); shared SCTP timer time is deduplicated. */
void rtc_sctp_tick(rtc_sctp_t *sctp);
void rtc_sctp_on_channel(rtc_sctp_t *sctp, rtc_sctp_on_channel_fn fn, void *user);
rtc_data_channel_t *rtc_sctp_create_channel(rtc_sctp_t *sctp, const char *label,
                                            const rtc_data_channel_init_t *options);

#endif
