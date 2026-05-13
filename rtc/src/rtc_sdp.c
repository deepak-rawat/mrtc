/*
 * rtc_sdp.c - SDP (RFC 4566) generation and parsing for WebRTC.
 *
 * Supports multiple m= lines: audio, video, and application (data channel).
 */
#include "rtc/rtc_sdp.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * Safe write helper: snprintf into (*p, *remain), advance pointers on success.
 * Returns RTC_OK on success, RTC_ERR_SDP if the buffer would overflow (in which
 * case *p and *remain are left unchanged). This avoids the unsigned underflow
 * that used to occur when a snprintf return value exceeded *remain and was
 * subtracted blindly from it.
 */
static int sdp_writef(char **p, size_t *remain, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(*p, *remain, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= *remain) {
        return RTC_ERR_SDP;
    }
    *p += n;
    *remain -= (size_t)n;
    return RTC_OK;
}

#define SDP_WRITE(p, r, ...)                         \
    do {                                             \
        int _rc = sdp_writef((p), (r), __VA_ARGS__); \
        if (_rc != RTC_OK)                           \
            return _rc;                              \
    } while (0)

/* Helper: write a single media section */
static int sdp_write_media_section(char **p, size_t *remain, const rtc_sdp_media_t *m,
                                   const rtc_sdp_t *sdp) {
    /* Media line */
    if (m->media_type == RTC_MEDIA_AUDIO) {
        SDP_WRITE(p, remain, "m=audio 9 UDP/TLS/RTP/SAVPF %d\r\n", m->payload_type);
    } else if (m->media_type == RTC_MEDIA_VIDEO) {
        SDP_WRITE(p, remain, "m=video 9 UDP/TLS/RTP/SAVPF %d\r\n", m->payload_type);
    } else {
        SDP_WRITE(p, remain, "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n");
    }

    SDP_WRITE(p, remain, "c=IN IP4 0.0.0.0\r\n");
    SDP_WRITE(p, remain, "a=mid:%d\r\n", m->mid_index);
    SDP_WRITE(p, remain, "a=ice-ufrag:%s\r\n", sdp->ice_ufrag);
    SDP_WRITE(p, remain, "a=ice-pwd:%s\r\n", sdp->ice_pwd);
    SDP_WRITE(p, remain, "a=fingerprint:sha-256 %s\r\n", sdp->fingerprint);

    const char *setup_str;
    switch (sdp->setup) {
        case RTC_SETUP_ACTIVE:
            setup_str = "active";
            break;
        case RTC_SETUP_PASSIVE:
            setup_str = "passive";
            break;
        default:
            setup_str = "actpass";
            break;
    }
    SDP_WRITE(p, remain, "a=setup:%s\r\n", setup_str);

    /* Direction (for RTP media) */
    if (m->media_type == RTC_MEDIA_AUDIO || m->media_type == RTC_MEDIA_VIDEO) {
        SDP_WRITE(p, remain, "a=sendrecv\r\n");
        SDP_WRITE(p, remain, "a=rtcp-mux\r\n");
    }

    /* Codec (for audio/video) */
    if ((m->media_type == RTC_MEDIA_AUDIO || m->media_type == RTC_MEDIA_VIDEO) &&
        m->codec_name[0]) {
        if (m->media_type == RTC_MEDIA_AUDIO && m->channels > 0) {
            SDP_WRITE(p, remain, "a=rtpmap:%d %s/%d/%d\r\n", m->payload_type, m->codec_name,
                      m->clockrate, m->channels);
        } else {
            SDP_WRITE(p, remain, "a=rtpmap:%d %s/%d\r\n", m->payload_type, m->codec_name,
                      m->clockrate);
        }
    }

    /* Data channel attributes */
    if (m->media_type == RTC_MEDIA_APPLICATION) {
        SDP_WRITE(p, remain, "a=sctp-port:5000\r\n");
    }

    return RTC_OK;
}

int rtc_sdp_generate(rtc_sdp_t *sdp) {
    char *p = sdp->raw;
    size_t remain = sizeof(sdp->raw);

    /* Session ID (use timestamp if not set) */
    if (sdp->session_id[0] == '\0') {
        snprintf(sdp->session_id, sizeof(sdp->session_id), "%llu",
                 (unsigned long long)rtc_time_ms());
    }

    SDP_WRITE(&p, &remain, "v=0\r\n");
    SDP_WRITE(&p, &remain, "o=mrtc %s 1 IN IP4 0.0.0.0\r\n", sdp->session_id);
    SDP_WRITE(&p, &remain, "s=mrtc\r\n");
    SDP_WRITE(&p, &remain, "t=0 0\r\n");

    /* Determine media count: use multi-media array if populated, else legacy single */
    int effective_media_count = sdp->media_count;
    rtc_sdp_media_t effective_media[SDP_MAX_MEDIA];

    if (effective_media_count > 0) {
        memcpy(effective_media, sdp->media,
               sizeof(rtc_sdp_media_t) * (size_t)effective_media_count);
    } else {
        /* Backward compatible: single media from legacy fields */
        effective_media_count = 1;
        memset(&effective_media[0], 0, sizeof(effective_media[0]));
        effective_media[0].media_type = sdp->media_type;
        effective_media[0].payload_type = sdp->payload_type;
        effective_media[0].clockrate = sdp->clockrate;
        effective_media[0].channels = sdp->channels;
        memcpy(effective_media[0].codec_name, sdp->codec_name, sizeof(sdp->codec_name));
        effective_media[0].mid_index = 0;
    }

    /* Bundle group */
    SDP_WRITE(&p, &remain, "a=group:BUNDLE");
    for (int i = 0; i < effective_media_count; i++) {
        SDP_WRITE(&p, &remain, " %d", effective_media[i].mid_index);
    }
    SDP_WRITE(&p, &remain, "\r\n");

    /* Write each media section */
    for (int i = 0; i < effective_media_count; i++) {
        int rc = sdp_write_media_section(&p, &remain, &effective_media[i], sdp);
        if (rc != RTC_OK)
            return rc;
    }

    /* Candidates (after all media sections — applies to BUNDLE) */
    size_t ncand = rtc_vec_len(&sdp->candidates);
    for (size_t i = 0; i < ncand; i++) {
        const rtc_ice_candidate_t *c =
            (const rtc_ice_candidate_t *)rtc_vec_at(&sdp->candidates, i);
        char ip[64];
        uint16_t port;
        if (rtc_addr_to_string(&c->addr, ip, sizeof(ip), &port) != RTC_OK)
            continue;

        const char *type_str;
        switch (c->type) {
            case ICE_CANDIDATE_HOST:
                type_str = "host";
                break;
            case ICE_CANDIDATE_SRFLX:
                type_str = "srflx";
                break;
            default:
                type_str = "host";
                break;
        }

        SDP_WRITE(&p, &remain, "a=candidate:%s %d udp %u %s %u typ %s\r\n", c->foundation,
                  c->component, c->priority, ip, port, type_str);
    }

    sdp->raw_len = (size_t)(p - sdp->raw);
    return RTC_OK;
}

/* Helper: safe line parsing */
static const char *sdp_next_line(const char *p, const char *end) {
    while (p < end && *p != '\n')
        p++;
    if (p < end)
        p++; /* skip \n */
    return p;
}

static bool sdp_line_starts(const char *line, const char *prefix) {
    return strncmp(line, prefix, strlen(prefix)) == 0;
}

/*
 * Length-aware prefix check: returns true only if the line is at least as long
 * as the prefix and matches it. This avoids the underflow that occurs when a
 * caller subtracts strlen(prefix) from line_len without first verifying that
 * line_len >= strlen(prefix).
 */
static bool sdp_line_starts_n(const char *line, size_t line_len, const char *prefix) {
    size_t plen = strlen(prefix);
    return line_len >= plen && strncmp(line, prefix, plen) == 0;
}

int rtc_sdp_parse(rtc_sdp_t *sdp, const char *text, size_t len) {
    /* Store raw text */
    if (len >= sizeof(sdp->raw))
        len = sizeof(sdp->raw) - 1;
    memcpy(sdp->raw, text, len);
    sdp->raw[len] = '\0';
    sdp->raw_len = len;

    /* Reset candidate list (lazy-init compatible). */
    if (sdp->candidates.elem_size == 0) {
        if (rtc_vec_init(&sdp->candidates, sizeof(rtc_ice_candidate_t)) != RTC_OK)
            return RTC_ERR_NOMEM;
    } else {
        rtc_vec_clear(&sdp->candidates);
    }
    sdp->media_count = 0;

    const char *p = text;
    const char *end = text + len;

    /* Index of the current m= section being parsed (-1 = session level) */
    int current_media = -1;

    while (p < end) {
        const char *line = p;
        const char *nl = p;
        while (nl < end && *nl != '\n' && *nl != '\r')
            nl++;
        size_t line_len = (size_t)(nl - line);

        /* Media type */
        if (sdp_line_starts(line, "m=audio")) {
            sdp->media_type = RTC_MEDIA_AUDIO;
            /* Parse payload type from m=audio 9 UDP/TLS/RTP/SAVPF <pt> */
            int pt = 0;
            if (sscanf(line, "m=audio %*d %*s %d", &pt) == 1)
                sdp->payload_type = pt;

            if (sdp->media_count < SDP_MAX_MEDIA) {
                current_media = sdp->media_count;
                rtc_sdp_media_t *m = &sdp->media[current_media];
                memset(m, 0, sizeof(*m));
                m->media_type = RTC_MEDIA_AUDIO;
                m->payload_type = pt;
                m->mid_index = current_media;
                sdp->media_count++;
            }
        } else if (sdp_line_starts(line, "m=video")) {
            sdp->media_type = RTC_MEDIA_VIDEO;
            int pt = 0;
            if (sscanf(line, "m=video %*d %*s %d", &pt) == 1)
                sdp->payload_type = pt;

            if (sdp->media_count < SDP_MAX_MEDIA) {
                current_media = sdp->media_count;
                rtc_sdp_media_t *m = &sdp->media[current_media];
                memset(m, 0, sizeof(*m));
                m->media_type = RTC_MEDIA_VIDEO;
                m->payload_type = pt;
                m->mid_index = current_media;
                sdp->media_count++;
            }
        } else if (sdp_line_starts(line, "m=application")) {
            sdp->media_type = RTC_MEDIA_APPLICATION;

            if (sdp->media_count < SDP_MAX_MEDIA) {
                current_media = sdp->media_count;
                rtc_sdp_media_t *m = &sdp->media[current_media];
                memset(m, 0, sizeof(*m));
                m->media_type = RTC_MEDIA_APPLICATION;
                m->mid_index = current_media;
                sdp->media_count++;
            }
        }

        /* ICE ufrag */
        if (sdp_line_starts_n(line, line_len, "a=ice-ufrag:")) {
            const char *val = line + 12;
            size_t vlen = line_len - 12;
            if (vlen >= ICE_UFRAG_LEN)
                vlen = ICE_UFRAG_LEN - 1;
            memcpy(sdp->ice_ufrag, val, vlen);
            sdp->ice_ufrag[vlen] = '\0';
        }

        /* ICE pwd */
        if (sdp_line_starts_n(line, line_len, "a=ice-pwd:")) {
            const char *val = line + 10;
            size_t vlen = line_len - 10;
            if (vlen >= ICE_PWD_LEN)
                vlen = ICE_PWD_LEN - 1;
            memcpy(sdp->ice_pwd, val, vlen);
            sdp->ice_pwd[vlen] = '\0';
        }

        /* Fingerprint */
        if (sdp_line_starts_n(line, line_len, "a=fingerprint:sha-256 ")) {
            const char *val = line + 22;
            size_t vlen = line_len - 22;
            if (vlen >= sizeof(sdp->fingerprint))
                vlen = sizeof(sdp->fingerprint) - 1;
            memcpy(sdp->fingerprint, val, vlen);
            sdp->fingerprint[vlen] = '\0';
        }

        /* Setup role */
        if (sdp_line_starts(line, "a=setup:")) {
            const char *val = line + 8;
            if (strncmp(val, "active", 6) == 0)
                sdp->setup = RTC_SETUP_ACTIVE;
            else if (strncmp(val, "passive", 7) == 0)
                sdp->setup = RTC_SETUP_PASSIVE;
            else
                sdp->setup = RTC_SETUP_ACTPASS;
        }

        /* mid */
        if (sdp_line_starts(line, "a=mid:") && current_media >= 0 &&
            current_media < sdp->media_count) {
            int mid_val = 0;
            if (sscanf(line, "a=mid:%d", &mid_val) == 1) {
                sdp->media[current_media].mid_index = mid_val;
            }
        }

        /* rtpmap */
        if (sdp_line_starts(line, "a=rtpmap:")) {
            int pt, rate, ch = 0;
            char codec[32] = {0};
            int matched = sscanf(line, "a=rtpmap:%d %31[^/]/%d/%d", &pt, codec, &rate, &ch);
            if (matched >= 3) {
                /* Set legacy fields */
                sdp->payload_type = pt;
                sdp->clockrate = rate;
                sdp->channels = ch;
                size_t clen = strlen(codec);
                if (clen >= sizeof(sdp->codec_name))
                    clen = sizeof(sdp->codec_name) - 1;
                memcpy(sdp->codec_name, codec, clen);
                sdp->codec_name[clen] = '\0';

                /* Set multi-media fields */
                if (current_media >= 0 && current_media < sdp->media_count) {
                    rtc_sdp_media_t *m = &sdp->media[current_media];
                    m->payload_type = pt;
                    m->clockrate = rate;
                    m->channels = ch;
                    if (clen >= sizeof(m->codec_name))
                        clen = sizeof(m->codec_name) - 1;
                    memcpy(m->codec_name, codec, clen);
                    m->codec_name[clen] = '\0';
                }
            }
        }

        /* Candidate */
        if (sdp_line_starts(line, "a=candidate:")) {
            rtc_ice_candidate_t c;
            memset(&c, 0, sizeof(c));
            char foundation[8] = {0}, transport[8] = {0}, type_str[8] = {0};
            char ip[64] = {0};
            int component = 0;
            unsigned int priority = 0, port = 0;

            /* a=candidate:foundation component transport priority ip port typ type */
            int parsed = sscanf(line, "a=candidate:%7s %d %7s %u %63s %u typ %7s", foundation,
                                &component, transport, &priority, ip, &port, type_str);

            if (parsed >= 7) {
                memcpy(c.foundation, foundation, sizeof(c.foundation));
                c.component = component;
                c.priority = priority;

                if (strcmp(type_str, "host") == 0)
                    c.type = ICE_CANDIDATE_HOST;
                else if (strcmp(type_str, "srflx") == 0)
                    c.type = ICE_CANDIDATE_SRFLX;
                else
                    c.type = ICE_CANDIDATE_HOST;

                rtc_addr_from_string(&c.addr, ip, (uint16_t)port);
                rtc_vec_push(&sdp->candidates, &c);
            }
        }

        p = sdp_next_line(p, end);
    }

    return RTC_OK;
}

void rtc_sdp_print(const rtc_sdp_t *sdp) {
    printf("=== SDP (%s) ===\n", sdp->type == RTC_SDP_OFFER    ? "offer"
                                 : sdp->type == RTC_SDP_ANSWER ? "answer"
                                                               : "pranswer");
    printf("%.*s", (int)sdp->raw_len, sdp->raw);
    printf("=== END SDP ===\n");
}

void rtc_sdp_close(rtc_sdp_t *sdp) {
    if (!sdp)
        return;
    rtc_vec_free(&sdp->candidates);
}

int rtc_sdp_add_candidate(rtc_sdp_t *sdp, const rtc_ice_candidate_t *c) {
    if (!sdp || !c)
        return RTC_ERR_INVALID;
    if (sdp->candidates.elem_size == 0) {
        rtc_err_t r = rtc_vec_init(&sdp->candidates, sizeof(rtc_ice_candidate_t));
        if (r != RTC_OK)
            return r;
    }
    return rtc_vec_push(&sdp->candidates, c);
}

size_t rtc_sdp_candidate_count(const rtc_sdp_t *sdp) {
    if (!sdp)
        return 0;
    return rtc_vec_len(&sdp->candidates);
}

const rtc_ice_candidate_t *rtc_sdp_get_candidate(const rtc_sdp_t *sdp, size_t idx) {
    if (!sdp)
        return NULL;
    return (const rtc_ice_candidate_t *)rtc_vec_at(&sdp->candidates, idx);
}
