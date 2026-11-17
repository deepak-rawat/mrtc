#ifndef RTC_SFU_H
#define RTC_SFU_H

#include <rtc/rtc_common.h>

#define RTC_SFU_ID_SIZE      16
#define RTC_SFU_ROOM_SIZE    64
#define RTC_SFU_SESSION_SIZE 64
#define RTC_SFU_MAX_PEERS    8
#define RTC_SFU_SDP_SIZE     8192

typedef struct rtc_sfu rtc_sfu_t;
typedef enum {
    RTC_SFU_OFFER,
    RTC_SFU_ANSWER,
    RTC_SFU_CANDIDATE,
    RTC_SFU_DATA,
    RTC_SFU_ERROR,
} rtc_sfu_event_type_t;

typedef struct {
    rtc_sfu_event_type_t type;
    char room[RTC_SFU_ROOM_SIZE];
    char owner[RTC_SFU_ID_SIZE];
    char remote[RTC_SFU_ID_SIZE];
    char session_id[RTC_SFU_SESSION_SIZE];
    uint32_t epoch;
    unsigned version;
    char text[RTC_SFU_SDP_SIZE];
    size_t data_len;
    uint8_t data[65535];
} rtc_sfu_event_t;

/* Called on the media worker. Copy the event into the control loop's queue. */
typedef void (*rtc_sfu_on_event_fn)(const rtc_sfu_event_t *event, void *user);
typedef int (*rtc_sfu_session_id_fn)(char out[RTC_SFU_SESSION_SIZE], uint32_t epoch,
                                     const char *subscriber, const char *publisher);

typedef struct {
    const char *listen_ip;
    const char *announced_ip;
    uint16_t port;
    rtc_sfu_on_event_fn on_event;
    rtc_sfu_session_id_fn subscription_id;
    void *user;
    /* Socket capacity; zero reserves two full single-room generations. */
    int max_io_sources;
} rtc_sfu_config_t;

typedef struct {
    rtc_sfu_event_type_t type;
    char room[RTC_SFU_ROOM_SIZE];
    char owner[RTC_SFU_ID_SIZE];
    char remote[RTC_SFU_ID_SIZE];
    char session_id[RTC_SFU_SESSION_SIZE];
    uint32_t epoch;
    unsigned version;
    char text[RTC_SFU_SDP_SIZE];
    char peers[RTC_SFU_MAX_PEERS][RTC_SFU_ID_SIZE];
    unsigned peer_count;
} rtc_sfu_command_t;

/* Serialize control calls on one thread. Initialize RTC/SCTP globally first;
 * close/destroy before global cleanup. Events run on the media worker. */
rtc_sfu_t *rtc_sfu_create(const rtc_sfu_config_t *config);
int rtc_sfu_handle(rtc_sfu_t *sfu, const rtc_sfu_command_t *command);
/* epoch == 0 removes all generations for this participant. */
void rtc_sfu_leave(rtc_sfu_t *sfu, const char *room, const char *owner, uint32_t epoch);
int rtc_sfu_send_data(rtc_sfu_t *sfu, const char *room, const char *owner, uint32_t epoch,
                      const uint8_t *data, size_t len);
void rtc_sfu_close(rtc_sfu_t *sfu);
void rtc_sfu_destroy(rtc_sfu_t *sfu);

#endif
