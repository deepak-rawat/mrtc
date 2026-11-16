#ifndef RTC_TURN_RUNTIME_H
#define RTC_TURN_RUNTIME_H

#include "rtc_packet_io.h"
#include "rtc_turn.h"
#include "rtc/rtc_worker.h"

typedef struct rtc_turn_runtime rtc_turn_runtime_t;

rtc_turn_runtime_t *rtc_turn_runtime_create(rtc_worker_t *worker, const rtc_turn_config_t *cfg,
                                            rtc_packet_io_recv_fn receive, void *user);
int rtc_turn_runtime_candidate(rtc_turn_runtime_t *turn, rtc_addr_t *relay);
int rtc_turn_runtime_set_peer(rtc_turn_runtime_t *turn, const rtc_addr_t *peer);
int rtc_turn_runtime_send(rtc_turn_runtime_t *turn, const uint8_t *data, size_t len,
                          const rtc_addr_t *peer);
void rtc_turn_runtime_destroy(rtc_turn_runtime_t *turn);

#endif
