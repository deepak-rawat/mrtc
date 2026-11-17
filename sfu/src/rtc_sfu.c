#include <rtc/rtc_sfu.h>

#include <rtc/rtc.h>
#include <rtc/rtc_consumer.h>
#include <rtc/rtc_listener.h>
#include <rtc/rtc_router.h>
#include <rtc/rtc_sdp.h>
#include <rtc/rtc_sctp.h>
#include <rtc/rtc_rtp_ext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct sfu_session {
    struct sfu_session *next;
    char owner[RTC_SFU_ID_SIZE];
    char publisher[RTC_SFU_ID_SIZE];
    char room[RTC_SFU_ROOM_SIZE];
    char session_id[RTC_SFU_SESSION_SIZE];
    uint32_t epoch;
    unsigned version;
    rtc_transport_t *transport;
    rtc_listener_t *listener;
    rtc_producer_t *producers[SDP_MAX_MEDIA];
    rtc_consumer_t *consumers[SDP_MAX_MEDIA];
    rtc_sdp_t description;
    rtc_sdp_t remote;
    rtc_producer_t *subscriptions[SDP_MAX_MEDIA];
    bool offer_pending;
    bool refresh_pending;
    uint8_t twcc_id;
    bool publishing;
    struct rtc_sfu *sfu;
    rtc_sctp_t *sctp;
    rtc_data_channel_t *channel;
    rtc_worker_timer_t timer;
    bool sctp_connected;
    bool media_ready;
    uint64_t last_stats_ms;
} sfu_session_t;

struct rtc_sfu {
    rtc_sfu_on_event_fn on_event;
    rtc_sfu_session_id_fn subscription_id;
    void *user;
    rtc_worker_t *worker;
    rtc_listener_t *listener;
    rtc_router_t *router;
    rtc_transport_t *control;
    sfu_session_t *sessions;
    char listen_ip[64];
    char announced_ip[64];
    bool trace;
    bool closed;
};

static int send_sctp(const uint8_t *data, size_t len, void *user) {
    sfu_session_t *session = user;
    return rtc_transport_send_data(session->transport, data, len);
}

static void on_sctp_data(const uint8_t *data, size_t len, void *user) {
    sfu_session_t *session = user;
    rtc_sctp_receive(session->sctp, data, len);
}

static void on_chat(const uint8_t *data, size_t len, void *user) {
    sfu_session_t *sender = user;
    if (len > sizeof(((rtc_sfu_event_t *)0)->data))
        return;
    rtc_sfu_event_t event = {
        .type = RTC_SFU_DATA, .epoch = sender->epoch, .version = sender->version, .data_len = len};
    snprintf(event.owner, sizeof(event.owner), "%s", sender->owner);
    snprintf(event.room, sizeof(event.room), "%s", sender->room);
    memcpy(event.data, data, len);
    sender->sfu->on_event(&event, sender->sfu->user);
}

static void on_channel(rtc_data_channel_t *channel, void *user) {
    sfu_session_t *session = user;
    session->channel = channel;
    rtc_data_channel_on_message(channel, on_chat, session);
}

static void session_tick(void *user) {
    sfu_session_t *session = user;
    rtc_transport_stats_t stats;
    if (rtc_transport_get_stats(session->transport, &stats) == RTC_OK) {
        if (session->sfu->trace && rtc_time_ms() - session->last_stats_ms >= 5000) {
            session->last_stats_ms = rtc_time_ms();
            for (int i = 0; i < SDP_MAX_MEDIA; i++) {
                rtc_producer_stats_t producer;
                rtc_consumer_stats_t consumer;
                if (session->producers[i] &&
                    rtc_producer_get_stats(session->producers[i], &producer) == RTC_OK)
                    RTC_LOG_INFO("SFU producer owner=%s kind=%d packets=%llu", session->owner,
                                 producer.kind, (unsigned long long)producer.packets_received);
                if (session->consumers[i] &&
                    rtc_consumer_get_stats(session->consumers[i], &consumer) == RTC_OK)
                    RTC_LOG_INFO("SFU consumer owner=%s kind=%d packets=%llu dropped=%llu",
                                 session->owner, consumer.kind,
                                 (unsigned long long)consumer.packets_sent,
                                 (unsigned long long)consumer.packets_dropped);
            }
        }
        if (stats.selected_tuple_valid && stats.dtls_state == RTC_TRANSPORT_DTLS_NEW)
            rtc_transport_start_dtls(session->transport);
        if (stats.srtp_ready && !session->media_ready) {
            RTC_LOG_INFO("SFU media ready owner=%s publisher=%s publishing=%d", session->owner,
                         session->publisher, session->publishing);
            session->media_ready = true;
            for (int i = 0; i < SDP_MAX_MEDIA; i++)
                if (session->consumers[i] && !session->offer_pending &&
                    session->description.media[i].direction == 1 &&
                    (session->remote.media[i].direction == 0 ||
                     session->remote.media[i].direction == 2))
                    rtc_consumer_resume(session->consumers[i]);
        }
        if (session->sctp && stats.dtls_state == RTC_TRANSPORT_DTLS_CONNECTED &&
            !session->sctp_connected) {
            rtc_dtls_parameters_t dtls;
            rtc_transport_get_dtls_parameters(session->transport, &dtls);
            int rc = rtc_sctp_connect(session->sctp, dtls.role == RTC_TRANSPORT_DTLS_ROLE_CLIENT);
            if (rc != RTC_OK)
                RTC_LOG_ERR("SFU SCTP association failed: %d", rc);
            else
                session->sctp_connected = true;
        }
        if (session->sctp && session->sctp_connected)
            rtc_sctp_tick(session->sctp);
    }
    session->timer =
        rtc_worker_add_timer(session->sfu->worker, rtc_time_ms() + 10, session_tick, session);
}

static void emit(sfu_session_t *session, rtc_sfu_event_type_t type, const char *text) {
    rtc_sfu_event_t event = {.type = type, .epoch = session->epoch, .version = session->version};
    snprintf(event.owner, sizeof(event.owner), "%s", session->owner);
    snprintf(event.room, sizeof(event.room), "%s", session->room);
    snprintf(event.remote, sizeof(event.remote), "%s",
             session->publishing ? "sfu" : session->publisher);
    snprintf(event.session_id, sizeof(event.session_id), "%s", session->session_id);
    snprintf(event.text, sizeof(event.text), "%s", text);
    session->sfu->on_event(&event, session->sfu->user);
}

static void send_candidates(sfu_session_t *session) {
    for (size_t i = 0; i < rtc_sdp_candidate_count(&session->description); i++) {
        char candidate[256];
        if (rtc_ice_candidate_to_string(rtc_sdp_get_candidate(&session->description, i), candidate,
                                        sizeof(candidate)) == RTC_OK)
            emit(session, RTC_SFU_CANDIDATE, candidate);
    }
    emit(session, RTC_SFU_CANDIDATE, "");
}

static sfu_session_t *find_session(rtc_sfu_t *sfu, const char *room, const char *owner,
                                   const char *publisher, uint32_t epoch) {
    for (sfu_session_t *s = sfu->sessions; s; s = s->next)
        if (s->epoch == epoch && strcmp(s->room, room) == 0 && strcmp(s->owner, owner) == 0 &&
            strcmp(s->publishing ? "sfu" : s->publisher, publisher) == 0)
            return s;
    return NULL;
}

static void destroy_session(sfu_session_t *session) {
    /* Stop callbacks before releasing any object reached from them. */
    rtc_worker_cancel_timer(session->sfu->worker, session->timer);
    rtc_transport_close(session->transport);
    rtc_sctp_destroy(session->sctp);
    for (int i = 0; i < SDP_MAX_MEDIA; i++)
        rtc_consumer_destroy(session->consumers[i]);
    for (int i = 0; i < SDP_MAX_MEDIA; i++)
        rtc_producer_destroy(session->producers[i]);
    rtc_transport_destroy(session->transport);
    if (session->listener != session->sfu->listener)
        rtc_listener_destroy(session->listener);
    rtc_sdp_close(&session->description);
    rtc_sdp_close(&session->remote);
    free(session);
}

static void on_feedback(const uint8_t *data, size_t len, void *user) {
    sfu_session_t *s = user;
    for (int i = 0; i < SDP_MAX_MEDIA; i++)
        if (s->consumers[i])
            rtc_consumer_handle_rtcp(s->consumers[i], data, len);
}

static sfu_session_t *new_session(rtc_sfu_t *sfu, const rtc_sfu_command_t *command,
                                  const char *publisher, bool publishing) {
    sfu_session_t *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    snprintf(s->owner, sizeof(s->owner), "%s", command->owner);
    snprintf(s->room, sizeof(s->room), "%s", command->room);
    snprintf(s->publisher, sizeof(s->publisher), "%s", publisher);
    s->epoch = command->epoch;
    s->version = command->version;
    if (publishing)
        snprintf(s->session_id, sizeof(s->session_id), "%s", command->session_id);
    else if (sfu->subscription_id(s->session_id, s->epoch, s->owner, s->publisher) != RTC_OK) {
        free(s);
        return NULL;
    }
    s->publishing = publishing;
    s->sfu = sfu;
    bool shared_listener = publishing;
    for (sfu_session_t *other = sfu->sessions; shared_listener && other; other = other->next)
        if (other->publishing && !strcmp(other->owner, command->owner) &&
            !strcmp(other->room, command->room))
            shared_listener = false;
    /* Overlapping generations may share the client's UDP socket. Distinct local
     * ports keep their DTLS associations from aliasing on the same tuple. */
    s->listener = shared_listener
                      ? sfu->listener
                      : rtc_listener_create(
                            sfu->worker, &(rtc_listener_config_t){.listen_ip = sfu->listen_ip,
                                                                  .announced_ip = sfu->announced_ip,
                                                                  .enable_udp = true});
    if (!s->listener) {
        free(s);
        return NULL;
    }
    s->transport = rtc_router_create_transport(
        sfu->router, &(rtc_transport_config_t){.listener = s->listener,
                                               .ice_mode = RTC_ICE_MODE_LITE,
                                               .enable_twcc = publishing,
                                               .initial_outgoing_bitrate_bps = 1500000});
    if (!s->transport) {
        if (s->listener != sfu->listener)
            rtc_listener_destroy(s->listener);
        free(s);
        return NULL;
    }
    if (!publishing)
        rtc_transport_on_rtcp(s->transport, on_feedback, s);
    s->timer = rtc_worker_add_timer(sfu->worker, rtc_time_ms() + 10, session_tick, s);
    return s;
}

static int fill_local(rtc_sfu_t *sfu, sfu_session_t *session, rtc_sdp_type_t type) {
    (void)sfu;
    rtc_sdp_t *sdp = &session->description;
    rtc_ice_parameters_t ice;
    rtc_dtls_parameters_t dtls;
    if (rtc_transport_get_ice_parameters(session->transport, &ice) != RTC_OK ||
        rtc_transport_get_dtls_parameters(session->transport, &dtls) != RTC_OK)
        return RTC_ERR_GENERIC;
    sdp->type = type;
    sdp->ice_lite = true;
    sdp->setup = type == RTC_SDP_OFFER                         ? RTC_SETUP_ACTPASS
                 : dtls.role == RTC_TRANSPORT_DTLS_ROLE_CLIENT ? RTC_SETUP_ACTIVE
                                                               : RTC_SETUP_PASSIVE;
    snprintf(sdp->ice_ufrag, sizeof(sdp->ice_ufrag), "%s", ice.username_fragment);
    snprintf(sdp->ice_pwd, sizeof(sdp->ice_pwd), "%s", ice.password);
    snprintf(sdp->fingerprint, sizeof(sdp->fingerprint), "%s", dtls.fingerprint);
    rtc_vec_clear(&sdp->candidates);
    if (rtc_listener_fill_sdp_candidates(session->listener, sdp) != RTC_OK)
        return RTC_ERR_GENERIC;
    int rc = rtc_sdp_generate(sdp);
    if (rc != RTC_OK || !session->publishing)
        return rc;
    char *section = sdp->raw;
    for (int i = 0; i < sdp->media_count; i++) {
        section = strstr(section, "m=");
        if (!section)
            return RTC_ERR_SDP;
        if (sdp->media[i].extmap_count) {
            char *extension = strstr(section, "a=extmap:");
            char feedback[64];
            int len = snprintf(feedback, sizeof(feedback), "a=rtcp-fb:%d transport-cc\r\n",
                               sdp->media[i].payload_type);
            if (!extension || len < 0 || (size_t)len >= sizeof(feedback) ||
                sdp->raw_len + (size_t)len >= sizeof(sdp->raw))
                return RTC_ERR_SDP;
            memmove(extension + len, extension, strlen(extension) + 1);
            memcpy(extension, feedback, (size_t)len);
            sdp->raw_len += (size_t)len;
        }
        section += 2;
    }
    return RTC_OK;
}

static int apply_remote(sfu_session_t *session, const rtc_sdp_t *remote) {
    rtc_ice_parameters_t ice = {0};
    rtc_dtls_parameters_t dtls = {0};
    if (!remote->ice_ufrag[0] || !remote->ice_pwd[0] || !remote->fingerprint[0])
        return RTC_ERR_SDP;
    snprintf(ice.username_fragment, sizeof(ice.username_fragment), "%s", remote->ice_ufrag);
    snprintf(ice.password, sizeof(ice.password), "%s", remote->ice_pwd);
    dtls.role = remote->setup == RTC_SETUP_PASSIVE ? RTC_TRANSPORT_DTLS_ROLE_SERVER
                                                   : RTC_TRANSPORT_DTLS_ROLE_CLIENT;
    snprintf(dtls.fingerprint, sizeof(dtls.fingerprint), "%s", remote->fingerprint);
    int rc = rtc_transport_set_remote_dtls_parameters(session->transport, &dtls);
    if (rc != RTC_OK)
        return rc;
    rc = rtc_transport_set_remote_ice_parameters(session->transport, &ice);
    if (rc != RTC_OK)
        return rc;
    for (size_t i = 0; i < rtc_sdp_candidate_count(remote); i++) {
        rtc_transport_candidate_t candidate;
        rc = rtc_transport_candidate_from_ice(&candidate, rtc_sdp_get_candidate(remote, i));
        if (rc != RTC_OK ||
            (rc = rtc_transport_add_remote_candidate(session->transport, &candidate)) != RTC_OK)
            return rc;
    }
    return RTC_OK; /* ICE-lite responds to the remote full agent's checks. */
}

static int add_trickled_candidate(sfu_session_t *session, const char *text) {
    if (!text[0])
        return RTC_OK;
    rtc_ice_candidate_t ice;
    rtc_transport_candidate_t candidate;
    int rc = rtc_sdp_parse_candidate_line(text, &ice);
    if (rc == RTC_OK)
        rc = rtc_transport_candidate_from_ice(&candidate, &ice);
    return rc == RTC_OK ? rtc_transport_add_remote_candidate(session->transport, &candidate) : rc;
}

static bool media_sending(const rtc_sdp_media_t *media) {
    return media->direction == 0 || media->direction == 1;
}

static int subscribe(rtc_sfu_t *sfu, const char *subscriber, sfu_session_t *publisher) {
    if (strcmp(subscriber, publisher->owner) == 0)
        return RTC_OK;
    sfu_session_t *s =
        find_session(sfu, publisher->room, subscriber, publisher->owner, publisher->epoch);
    bool created = s == NULL;
    if (s && s->offer_pending) {
        int index = 0;
        for (int i = 0; i < publisher->remote.media_count; i++) {
            if (publisher->remote.media[i].media_type == RTC_MEDIA_APPLICATION)
                continue;
            if (!media_sending(&publisher->remote.media[i]) && s->consumers[index])
                rtc_consumer_pause(s->consumers[index]);
            index++;
        }
        s->refresh_pending = true;
        return RTC_OK;
    }
    if (!s) {
        rtc_sfu_command_t command = {.epoch = publisher->epoch, .version = publisher->version};
        snprintf(command.owner, sizeof(command.owner), "%s", subscriber);
        snprintf(command.room, sizeof(command.room), "%s", publisher->room);
        s = new_session(sfu, &command, publisher->owner, false);
    }
    if (!s) {
        RTC_LOG_ERR("SFU subscription session creation failed: subscriber=%s publisher=%s",
                    subscriber, publisher->owner);
        return RTC_ERR_NOMEM;
    }
    s->description.media_count = 0;
    for (int i = 0; i < publisher->description.media_count; i++) {
        if (publisher->description.media[i].media_type == RTC_MEDIA_APPLICATION)
            continue;
        int index = s->description.media_count++;
        s->description.media[index] = publisher->description.media[i];
        s->description.media[index].mid_index = index;
        bool sending = media_sending(&publisher->remote.media[i]);
        s->description.media[index].direction = sending ? 1 : 3;
        s->description.media[index].extmap_count = 0;
        if (s->subscriptions[index] != publisher->producers[i]) {
            rtc_consumer_destroy(s->consumers[index]);
            s->consumers[index] = NULL;
            s->subscriptions[index] = publisher->producers[i];
            if (publisher->producers[i])
                s->consumers[index] = rtc_transport_consume(
                    s->transport,
                    &(rtc_consumer_options_t){.producer = publisher->producers[i], .paused = true});
            if (publisher->producers[i] && !s->consumers[index]) {
                RTC_LOG_ERR("SFU consumer creation failed: subscriber=%s publisher=%s media=%d",
                            subscriber, publisher->owner, i);
                if (created)
                    destroy_session(s);
                return RTC_ERR_GENERIC;
            }
        }
        if (s->consumers[index]) {
            rtc_rtp_parameters_t rtp;
            if (rtc_consumer_get_rtp_parameters(s->consumers[index], &rtp) != RTC_OK)
                return RTC_ERR_GENERIC;
            s->description.media[index].ssrc = rtp.ssrc;
            if (!sending)
                rtc_consumer_pause(s->consumers[index]);
        }
    }
    /* A data-only publisher has no downstream RTP session to negotiate. */
    if (!s->description.media_count) {
        if (created)
            destroy_session(s);
        return RTC_OK;
    }
    int rc = fill_local(sfu, s, RTC_SDP_OFFER);
    if (rc != RTC_OK) {
        RTC_LOG_ERR("SFU subscription SDP failed: subscriber=%s publisher=%s error=%d", subscriber,
                    publisher->owner, rc);
        if (created)
            destroy_session(s);
        return rc;
    }
    if (created) {
        s->next = sfu->sessions;
        sfu->sessions = s;
    }
    s->offer_pending = true;
    emit(s, RTC_SFU_OFFER, s->description.raw);
    send_candidates(s);
    return RTC_OK;
}

static int validate_publisher(sfu_session_t *session, const rtc_sdp_t *remote, uint8_t *twcc_id) {
    if (!remote->ice_ufrag[0] || !remote->ice_pwd[0] || !remote->fingerprint[0] ||
        !remote->media_count ||
        (session &&
         (remote->media_count < session->remote.media_count ||
          strcmp(remote->fingerprint, session->remote.fingerprint) != 0 ||
          (remote->setup == RTC_SETUP_PASSIVE) != (session->remote.setup == RTC_SETUP_PASSIVE))))
        return RTC_ERR_SDP;
    int applications = 0;
    *twcc_id = 0;
    for (int i = 0; i < remote->media_count; i++) {
        const rtc_sdp_media_t *media = &remote->media[i];
        const rtc_sdp_media_t *old =
            session && i < session->remote.media_count ? &session->remote.media[i] : NULL;
        for (int j = 0; j < i; j++)
            if (remote->media[j].mid_index == media->mid_index ||
                (media->ssrc && remote->media[j].ssrc == media->ssrc))
                return RTC_ERR_SDP;
        if (session && media->ssrc)
            for (int j = 0; j < session->remote.media_count; j++)
                if (i != j && session->remote.media[j].ssrc == media->ssrc)
                    return RTC_ERR_SDP;
        if (old && (old->mid_index != media->mid_index || old->media_type != media->media_type))
            return RTC_ERR_SDP;
        if (media->media_type == RTC_MEDIA_APPLICATION) {
            if (++applications > 1 || media->sctp_port != 5000 ||
                (old && old->sctp_port != media->sctp_port))
                return RTC_ERR_SDP;
            continue;
        }
        bool video = media->media_type == RTC_MEDIA_VIDEO;
        if (strcmp(media->codec_name, video ? "VP8" : "opus") != 0 || media->payload_type < 0 ||
            media->payload_type > 127 || media->clockrate != (video ? 90000 : 48000) ||
            (media_sending(media) && !media->ssrc) ||
            (old && (old->payload_type != media->payload_type ||
                     strcmp(old->codec_name, media->codec_name) != 0 ||
                     old->clockrate != media->clockrate || old->channels != media->channels)))
            return RTC_ERR_SDP;
#ifdef MRTC_ENABLE_TWCC
        if (video && !*twcc_id)
            *twcc_id = rtc_sdp_media_find_extmap_id(media, RTC_EXT_URI_TRANSPORT_CC);
#endif
    }
    if (*twcc_id > 14 || (session && session->twcc_id && *twcc_id && session->twcc_id != *twcc_id))
        return RTC_ERR_SDP;
    return RTC_OK;
}

static int publish(rtc_sfu_t *sfu, const rtc_sfu_command_t *command) {
    const char *peer = command->owner;
    sfu_session_t *s = find_session(sfu, command->room, peer, "sfu", command->epoch);
    bool created = s == NULL;
    rtc_sdp_t remote = {0};
    int rc = rtc_sdp_parse(&remote, command->text, strlen(command->text));
    uint8_t twcc_id = 0;
    if (rc == RTC_OK)
        rc = validate_publisher(s, &remote, &twcc_id);
    if (rc != RTC_OK) {
        rtc_sdp_close(&remote);
        return rc;
    }
    if (!s)
        s = new_session(sfu, command, peer, true);
    if (!s) {
        rtc_sdp_close(&remote);
        return RTC_ERR_NOMEM;
    }
    rtc_producer_t *staged[SDP_MAX_MEDIA] = {0};
    bool replaced[SDP_MAX_MEDIA] = {0};
    for (int i = 0; i < remote.media_count; i++) {
        rtc_sdp_media_t *m = &remote.media[i];
        if (m->media_type == RTC_MEDIA_APPLICATION) {
            if (!s->sctp)
                s->sctp = rtc_sctp_create(send_sctp, s);
            if (!s->sctp) {
                rc = RTC_ERR_GENERIC;
                goto fail;
            }
            rtc_sctp_on_channel(s->sctp, on_channel, s);
            rtc_transport_on_data(s->transport, on_sctp_data, s);
            continue;
        }
        bool video = m->media_type == RTC_MEDIA_VIDEO;
        staged[i] = s->producers[i];
        if (!media_sending(m))
            continue;
        rtc_rtp_parameters_t previous;
        if (staged[i] && rtc_producer_get_rtp_parameters(staged[i], &previous) == RTC_OK &&
            previous.ssrc == m->ssrc) {
            continue;
        }
        rtc_producer_options_t opts = {0};
        opts.kind = video ? RTC_MEDIA_KIND_VIDEO : RTC_MEDIA_KIND_AUDIO;
        opts.label = video ? "camera" : "mic";
        opts.rtp.ssrc = m->ssrc;
        opts.rtp.codec_count = 1;
        opts.rtp.codecs[0].kind = opts.kind;
        opts.rtp.codecs[0].payload_type = (uint8_t)m->payload_type;
        opts.rtp.codecs[0].clock_rate = (uint32_t)m->clockrate;
        opts.rtp.codecs[0].channels = m->channels;
        snprintf(opts.rtp.codecs[0].mime_type, sizeof(opts.rtp.codecs[0].mime_type), "%s/%s",
                 video ? "video" : "audio", m->codec_name);
        staged[i] = rtc_transport_produce(s->transport, &opts);
        replaced[i] = true;
        if (!staged[i]) {
            rc = RTC_ERR_GENERIC;
            goto fail;
        }
    }
    bool restart = !created && (strcmp(remote.ice_ufrag, s->remote.ice_ufrag) != 0 ||
                                strcmp(remote.ice_pwd, s->remote.ice_pwd) != 0);
    if (restart && (rc = rtc_transport_restart_ice(s->transport)) != RTC_OK)
        goto fail;
    rc = apply_remote(s, &remote);
    if (rc != RTC_OK)
        goto fail;
#ifdef MRTC_ENABLE_TWCC
    if (twcc_id && (rc = rtc_transport_enable_twcc(s->transport, twcc_id, 1)) != RTC_OK)
        goto fail;
#endif
    /* Detach downstream consumers before retiring a replaced SSRC's producer. */
    for (int i = 0; i < SDP_MAX_MEDIA; i++) {
        if (!replaced[i])
            continue;
        for (sfu_session_t *sub = sfu->sessions; sub; sub = sub->next)
            for (int j = 0; !sub->publishing && j < SDP_MAX_MEDIA; j++)
                if (sub->subscriptions[j] && sub->subscriptions[j] == s->producers[i]) {
                    rtc_consumer_destroy(sub->consumers[j]);
                    sub->consumers[j] = NULL;
                    sub->subscriptions[j] = NULL;
                }
        rtc_producer_destroy(s->producers[i]);
        s->producers[i] = staged[i];
        replaced[i] = false;
    }
    rtc_sdp_close(&s->remote);
    s->remote = remote;
    memset(&remote, 0, sizeof(remote));
    s->description.media_count = s->remote.media_count;
    for (int i = 0; i < s->remote.media_count; i++) {
        const rtc_sdp_media_t *m = &s->remote.media[i];
        s->description.media[i] = *m;
        if (m->media_type == RTC_MEDIA_APPLICATION) {
            s->description.media[i].max_message_size = 65536;
            continue;
        }
        s->description.media[i].direction = media_sending(m) ? 2 : 3;
        s->description.media[i].extmap_count = 0;
        if (twcc_id && rtc_sdp_media_find_extmap_id(m, RTC_EXT_URI_TRANSPORT_CC) == twcc_id)
            rtc_sdp_media_add_extmap(&s->description.media[i], twcc_id, RTC_EXT_URI_TRANSPORT_CC);
        s->description.media[i].ssrc = 0;
    }
    if (twcc_id)
        s->twcc_id = twcc_id;
    rc = fill_local(sfu, s, RTC_SDP_ANSWER);
    if (rc != RTC_OK)
        goto fail;
    if (created) {
        s->next = sfu->sessions;
        sfu->sessions = s;
    }
    emit(s, RTC_SFU_ANSWER, s->description.raw);
    send_candidates(s);
    for (unsigned i = 0; i < command->peer_count; i++)
        if (subscribe(sfu, command->peers[i], s) != RTC_OK)
            emit(s, RTC_SFU_ERROR, "subscription creation failed");
    if (created)
        for (sfu_session_t *p = sfu->sessions; p; p = p->next)
            if (p->publishing && p != s && p->epoch == command->epoch &&
                strcmp(p->room, command->room) == 0)
                if (subscribe(sfu, peer, p) != RTC_OK)
                    emit(s, RTC_SFU_ERROR, "subscription creation failed");
    return RTC_OK;
fail:
    for (int i = 0; i < SDP_MAX_MEDIA; i++)
        if (replaced[i])
            rtc_producer_destroy(staged[i]);
    rtc_sdp_close(&remote);
    if (created)
        destroy_session(s);
    return rc;
}

rtc_sfu_t *rtc_sfu_create(const rtc_sfu_config_t *config) {
    if (!config || !config->listen_ip || !config->on_event || !config->subscription_id)
        return NULL;
    const char *listen_ip = config->listen_ip;
    const char *announced_ip = config->announced_ip;
    uint16_t port = config->port;
    rtc_sfu_t *sfu = calloc(1, sizeof(*sfu));
    if (!sfu)
        return NULL;
    sfu->on_event = config->on_event;
    sfu->subscription_id = config->subscription_id;
    sfu->user = config->user;
    sfu->trace = getenv("MRTC_SFU_TRACE") != NULL;
    snprintf(sfu->listen_ip, sizeof(sfu->listen_ip), "%s", listen_ip);
    snprintf(sfu->announced_ip, sizeof(sfu->announced_ip), "%s",
             announced_ip ? announced_ip : listen_ip);
    sfu->worker = rtc_worker_create(&(rtc_worker_config_t){
        .max_io_sources = config->max_io_sources > 0
                              ? config->max_io_sources
                              : 1 + 2 * RTC_SFU_MAX_PEERS * RTC_SFU_MAX_PEERS});
    if (sfu->worker)
        sfu->listener = rtc_listener_create(
            sfu->worker,
            &(rtc_listener_config_t){.listen_ip = listen_ip,
                                     .announced_ip = announced_ip ? announced_ip : listen_ip,
                                     .udp_port = port,
                                     .enable_udp = true});
    if (sfu->listener) {
        sfu->router = rtc_router_create(sfu->worker, NULL);
        sfu->control =
            rtc_transport_create(sfu->worker, &(rtc_transport_config_t){.listener = sfu->listener});
    }
    if (!sfu->router || !sfu->control) {
        rtc_sfu_destroy(sfu);
        return NULL;
    }
    return sfu;
}

static int handle_worker(rtc_sfu_t *sfu, const rtc_sfu_command_t *message) {
    if (!sfu || !message->owner[0] || !message->room[0])
        return RTC_ERR_INVALID;
    if (message->type == RTC_SFU_OFFER && strcmp(message->remote, "sfu") == 0)
        return publish(sfu, message);
    sfu_session_t *s =
        find_session(sfu, message->room, message->owner, message->remote, message->epoch);
    if (!s)
        return RTC_ERR_INVALID;
    if (message->session_id[0] && strcmp(s->session_id, message->session_id) != 0)
        return RTC_ERR_INVALID;
    if (message->type == RTC_SFU_ANSWER && !s->publishing) {
        if (!s->offer_pending)
            return RTC_ERR_INVALID;
        rtc_sdp_t remote = {0};
        int rc = rtc_sdp_parse(&remote, message->text, strlen(message->text));
        if (rc == RTC_OK &&
            (remote.media_count != s->description.media_count ||
             (s->remote.fingerprint[0] && strcmp(s->remote.fingerprint, remote.fingerprint) != 0)))
            rc = RTC_ERR_SDP;
        for (int i = 0; rc == RTC_OK && i < remote.media_count; i++)
            if (remote.media[i].mid_index != s->description.media[i].mid_index ||
                remote.media[i].media_type != s->description.media[i].media_type ||
                remote.media[i].payload_type != s->description.media[i].payload_type ||
                strcmp(remote.media[i].codec_name, s->description.media[i].codec_name) != 0)
                rc = RTC_ERR_SDP;
        if (rc == RTC_OK)
            rc = apply_remote(s, &remote);
        if (rc == RTC_OK) {
            rtc_sdp_close(&s->remote);
            s->remote = remote;
            memset(&remote, 0, sizeof(remote));
            s->offer_pending = false;
            rtc_transport_stats_t stats;
            bool ready =
                rtc_transport_get_stats(s->transport, &stats) == RTC_OK && stats.srtp_ready;
            for (int i = 0; i < s->description.media_count; i++) {
                if (!s->consumers[i])
                    continue;
                if (ready && s->description.media[i].direction == 1 &&
                    (s->remote.media[i].direction == 0 || s->remote.media[i].direction == 2))
                    rtc_consumer_resume(s->consumers[i]);
                else
                    rtc_consumer_pause(s->consumers[i]);
            }
            if (s->refresh_pending) {
                s->refresh_pending = false;
                sfu_session_t *publisher =
                    find_session(sfu, s->room, s->publisher, "sfu", s->epoch);
                rc = publisher ? subscribe(sfu, s->owner, publisher) : RTC_ERR_INVALID;
            }
        }
        rtc_sdp_close(&remote);
        return rc;
    }
    if (message->type == RTC_SFU_CANDIDATE) {
        return add_trickled_candidate(s, message->text);
    }
    return RTC_ERR_INVALID;
}

static void leave_worker(rtc_sfu_t *sfu, const char *room, const char *peer, uint32_t epoch) {
    if (!sfu)
        return;
    /* Consumers must be removed before the corresponding producer. */
    for (int publishing = 0; publishing <= 1; publishing++) {
        sfu_session_t **cursor = &sfu->sessions;
        while (*cursor) {
            sfu_session_t *s = *cursor;
            if ((int)s->publishing == publishing && strcmp(s->room, room) == 0 &&
                (!epoch || s->epoch == epoch) &&
                (strcmp(s->owner, peer) == 0 || strcmp(s->publisher, peer) == 0)) {
                *cursor = s->next;
                destroy_session(s);
            } else {
                cursor = &s->next;
            }
        }
    }
}

typedef struct {
    rtc_sfu_t *sfu;
    rtc_sfu_command_t message;
    bool leave;
    bool data;
    size_t len;
    uint8_t bytes[65535];
    int result;
} sfu_command_t;

static void handle_command(void *user) {
    sfu_command_t *command = user;
    if (command->leave)
        leave_worker(command->sfu, command->message.room, command->message.owner,
                     command->message.epoch);
    else if (command->data) {
        sfu_session_t *session =
            find_session(command->sfu, command->message.room, command->message.owner, "sfu",
                         command->message.epoch);
        command->result =
            session && session->channel
                ? rtc_data_channel_send(session->channel, command->bytes, command->len)
                : RTC_ERR_INVALID;
    } else
        command->result = handle_worker(command->sfu, &command->message);
}

int rtc_sfu_handle(rtc_sfu_t *sfu, const rtc_sfu_command_t *message) {
    if (!sfu || sfu->closed || !message || message->peer_count > RTC_SFU_MAX_PEERS ||
        !message->epoch || !memchr(message->room, 0, sizeof(message->room)) ||
        !memchr(message->owner, 0, sizeof(message->owner)) ||
        !memchr(message->remote, 0, sizeof(message->remote)) ||
        !memchr(message->session_id, 0, sizeof(message->session_id)) ||
        !memchr(message->text, 0, sizeof(message->text)))
        return RTC_ERR_INVALID;
    for (unsigned i = 0; i < message->peer_count; i++)
        if (!message->peers[i][0] || !memchr(message->peers[i], 0, sizeof(message->peers[i])))
            return RTC_ERR_INVALID;
    sfu_command_t command = {.sfu = sfu, .message = *message};
    int rc = rtc_transport_invoke(sfu->control, handle_command, &command);
    return rc == RTC_OK ? command.result : rc;
}

void rtc_sfu_leave(rtc_sfu_t *sfu, const char *room, const char *owner, uint32_t epoch) {
    if (!sfu || sfu->closed || !room || !owner)
        return;
    sfu_command_t command = {.sfu = sfu, .leave = true};
    snprintf(command.message.room, sizeof(command.message.room), "%s", room);
    snprintf(command.message.owner, sizeof(command.message.owner), "%s", owner);
    command.message.epoch = epoch;
    rtc_transport_invoke(sfu->control, handle_command, &command);
}

int rtc_sfu_send_data(rtc_sfu_t *sfu, const char *room, const char *owner, uint32_t epoch,
                      const uint8_t *data, size_t len) {
    if (!sfu || sfu->closed || !room || !owner || (!data && len) || len > 65535)
        return RTC_ERR_INVALID;
    sfu_command_t command = {.sfu = sfu, .data = true, .len = len};
    snprintf(command.message.room, sizeof(command.message.room), "%s", room);
    snprintf(command.message.owner, sizeof(command.message.owner), "%s", owner);
    command.message.epoch = epoch;
    if (len)
        memcpy(command.bytes, data, len);
    int rc = rtc_transport_invoke(sfu->control, handle_command, &command);
    return rc == RTC_OK ? command.result : rc;
}

void rtc_sfu_close(rtc_sfu_t *sfu) {
    if (!sfu || sfu->closed)
        return;
    while (sfu->sessions)
        rtc_sfu_leave(sfu, sfu->sessions->room, sfu->sessions->owner, 0);
    sfu->closed = true;
    rtc_transport_close(sfu->control);
    rtc_router_close(sfu->router);
    rtc_listener_close(sfu->listener);
    rtc_worker_close(sfu->worker);
}

void rtc_sfu_destroy(rtc_sfu_t *sfu) {
    if (!sfu)
        return;
    rtc_sfu_close(sfu);
    rtc_transport_destroy(sfu->control);
    rtc_router_destroy(sfu->router);
    rtc_listener_destroy(sfu->listener);
    rtc_worker_destroy(sfu->worker);
    free(sfu);
}
