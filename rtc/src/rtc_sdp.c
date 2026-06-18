/*
 * SDP (RFC 4566) generation and parsing for WebRTC.
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

    /* RTP header extensions (RFC 8285 a=extmap) — audio/video only */
    if (m->media_type == RTC_MEDIA_AUDIO || m->media_type == RTC_MEDIA_VIDEO) {
        for (int i = 0; i < m->extmap_count; i++) {
            if (m->extmaps[i].id == 0 || m->extmaps[i].uri[0] == '\0')
                continue;
            SDP_WRITE(p, remain, "a=extmap:%u %s\r\n", (unsigned)m->extmaps[i].id,
                      m->extmaps[i].uri);
        }
    }

    /* Data channel attributes */
    if (m->media_type == RTC_MEDIA_APPLICATION) {
        SDP_WRITE(p, remain, "a=sctp-port:5000\r\n");
    }

    /* SSRC (audio/video only). cname is required by RFC 5576; mrtc does not
     * use it for correlation so a fixed token suffices. */
    if ((m->media_type == RTC_MEDIA_AUDIO || m->media_type == RTC_MEDIA_VIDEO) && m->ssrc != 0) {
        SDP_WRITE(p, remain, "a=ssrc:%u cname:mrtc\r\n", m->ssrc);
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

    /* Advertise trickle ICE support (RFC 8839 §5.4). Always emit — mrtc
     * gathers synchronously today but accepts add_ice_candidate calls. */
    SDP_WRITE(&p, &remain, "a=ice-options:trickle\r\n");
    sdp->ice_options_trickle = true;

    /* Write each media section */
    for (int i = 0; i < effective_media_count; i++) {
        int rc = sdp_write_media_section(&p, &remain, &effective_media[i], sdp);
        if (rc != RTC_OK)
            return rc;
    }

    /* Candidates (after all media sections — applies to BUNDLE) */
    size_t ncand = rtc_vec_len(&sdp->candidates);
    for (size_t i = 0; i < ncand; i++) {
        const rtc_ice_candidate_t *c = (const rtc_ice_candidate_t *)rtc_vec_at(&sdp->candidates, i);
        char cand_line[256];
        if (rtc_ice_candidate_to_string(c, cand_line, sizeof(cand_line)) != RTC_OK)
            continue;
        SDP_WRITE(&p, &remain, "a=%s\r\n", cand_line);
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

int rtc_sdp_parse_candidate_line(const char *line, rtc_ice_candidate_t *out) {
    if (!line || !out)
        return RTC_ERR_INVALID;

    const char *p = line;
    if (strncmp(p, "a=", 2) == 0)
        p += 2;
    if (strncmp(p, "candidate:", 10) != 0)
        return RTC_ERR_SDP;
    p += 10;

    rtc_ice_candidate_t c;
    memset(&c, 0, sizeof(c));
    char foundation[32] = {0}, transport[8] = {0}, type_str[8] = {0};
    char ip[64] = {0};
    int component = 0;
    unsigned int priority = 0, port = 0;

    int parsed = sscanf(p, "%31s %d %7s %u %63s %u typ %7s", foundation, &component, transport,
                        &priority, ip, &port, type_str);
    if (parsed < 7)
        return RTC_ERR_SDP;
    if (strcmp(transport, "udp") != 0 && strcmp(transport, "UDP") != 0)
        return RTC_ERR_SDP;
    if (component <= 0 || port == 0 || port > 65535)
        return RTC_ERR_SDP;

    memcpy(c.foundation, foundation, sizeof(c.foundation));
    c.component = component;
    c.priority = priority;

    if (strcmp(type_str, "host") == 0)
        c.type = ICE_CANDIDATE_HOST;
    else if (strcmp(type_str, "srflx") == 0)
        c.type = ICE_CANDIDATE_SRFLX;
    else if (strcmp(type_str, "relay") == 0)
        c.type = ICE_CANDIDATE_RELAY;
    else
        return RTC_ERR_SDP;

    int rc = rtc_addr_from_string(&c.addr, ip, (uint16_t)port);
    if (rc != RTC_OK)
        return rc;

    const char *related = strstr(p, " raddr ");
    if (related) {
        char related_ip[64] = {0};
        unsigned int related_port = 0;
        if (sscanf(related, " raddr %63s rport %u", related_ip, &related_port) == 2) {
            if (related_port == 0 || related_port > 65535)
                return RTC_ERR_SDP;
            rc = rtc_addr_from_string(&c.related_addr, related_ip, (uint16_t)related_port);
            if (rc != RTC_OK)
                return rc;
            c.has_related_addr = true;
        }
    }

    *out = c;
    return RTC_OK;
}

uint32_t rtc_ice_candidate_priority(rtc_ice_candidate_type_t type, int local_pref, int component) {
    uint32_t type_pref;
    switch (type) {
        case ICE_CANDIDATE_HOST:
            type_pref = 126;
            break;
        case ICE_CANDIDATE_SRFLX:
            type_pref = 100;
            break;
        case ICE_CANDIDATE_RELAY:
            type_pref = 0;
            break;
        default:
            type_pref = 0;
            break;
    }
    if (local_pref < 0)
        local_pref = 0;
    if (local_pref > 65535)
        local_pref = 65535;
    if (component < 1)
        component = 1;
    if (component > 255)
        component = 255;
    return (type_pref << 24) | ((uint32_t)local_pref << 8) | (256u - (uint32_t)component);
}

int rtc_ice_candidate_to_string(const rtc_ice_candidate_t *candidate, char *buf, size_t len) {
    if (!candidate || !buf || len == 0)
        return RTC_ERR_INVALID;

    char ip[64];
    uint16_t port = 0;
    int rc = rtc_addr_to_string(&candidate->addr, ip, sizeof(ip), &port);
    if (rc != RTC_OK)
        return rc;

    const char *type_str;
    switch (candidate->type) {
        case ICE_CANDIDATE_HOST:
            type_str = "host";
            break;
        case ICE_CANDIDATE_SRFLX:
            type_str = "srflx";
            break;
        case ICE_CANDIDATE_RELAY:
            type_str = "relay";
            break;
        default:
            return RTC_ERR_INVALID;
    }

    int n = snprintf(buf, len, "candidate:%s %d udp %u %s %u typ %s", candidate->foundation,
                     candidate->component, candidate->priority, ip, port, type_str);
    if (n < 0 || (size_t)n >= len)
        return RTC_ERR_NOMEM;

    if (candidate->has_related_addr) {
        char related_ip[64];
        uint16_t related_port = 0;
        rc = rtc_addr_to_string(&candidate->related_addr, related_ip, sizeof(related_ip),
                                &related_port);
        if (rc != RTC_OK)
            return rc;
        size_t used = (size_t)n;
        int rn = snprintf(buf + used, len - used, " raddr %s rport %u", related_ip, related_port);
        if (rn < 0 || (size_t)rn >= len - used)
            return RTC_ERR_NOMEM;
    }

    return RTC_OK;
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

        /* ice-options (RFC 8839): note trickle support if advertised. */
        if (sdp_line_starts_n(line, line_len, "a=ice-options:")) {
            const char *val = line + 14;
            size_t vlen = line_len - 14;
            /* Scan space-separated tokens for "trickle". */
            const char *q = val;
            const char *qend = val + vlen;
            while (q < qend) {
                while (q < qend && (*q == ' ' || *q == '\t'))
                    q++;
                const char *tok = q;
                while (q < qend && *q != ' ' && *q != '\t')
                    q++;
                size_t toklen = (size_t)(q - tok);
                if (toklen == 7 && strncmp(tok, "trickle", 7) == 0) {
                    sdp->ice_options_trickle = true;
                    break;
                }
            }
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

        /* SSRC: a=ssrc:<NNN> ... (only first ssrc per m= section is captured;
         * RFC 5576 allows multiple ssrc lines for FEC/RTX, those are ignored). */
        if (sdp_line_starts(line, "a=ssrc:") && current_media >= 0 &&
            current_media < sdp->media_count) {
            unsigned long ssrc_val = 0;
            if (sscanf(line, "a=ssrc:%lu", &ssrc_val) == 1 && sdp->media[current_media].ssrc == 0) {
                sdp->media[current_media].ssrc = (uint32_t)ssrc_val;
            }
        }

        /* extmap: a=extmap:<id>[/dir] <uri> */
        if (sdp_line_starts(line, "a=extmap:") && current_media >= 0 &&
            current_media < sdp->media_count) {
            int id_val = 0;
            char uri[SDP_EXTMAP_URI_LEN] = {0};
            if (sscanf(line, "a=extmap:%d %95s", &id_val, uri) == 2 ||
                sscanf(line, "a=extmap:%d/%*[^ ] %95s", &id_val, uri) == 2) {
                rtc_sdp_media_t *m = &sdp->media[current_media];
                if (id_val >= 1 && id_val <= 14 && m->extmap_count < SDP_MAX_EXTMAP) {
                    m->extmaps[m->extmap_count].id = (uint8_t)id_val;
                    size_t ulen = strlen(uri);
                    if (ulen >= sizeof(m->extmaps[m->extmap_count].uri))
                        ulen = sizeof(m->extmaps[m->extmap_count].uri) - 1;
                    memcpy(m->extmaps[m->extmap_count].uri, uri, ulen);
                    m->extmaps[m->extmap_count].uri[ulen] = '\0';
                    m->extmap_count++;
                }
            }
        }

        /* Candidate */
        if (sdp_line_starts(line, "a=candidate:")) {
            rtc_ice_candidate_t c;
            if (rtc_sdp_parse_candidate_line(line, &c) == RTC_OK)
                rtc_vec_push(&sdp->candidates, &c);
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

int rtc_sdp_media_add_extmap(rtc_sdp_media_t *m, uint8_t id, const char *uri) {
    if (!m || !uri || id < 1 || id > 14)
        return RTC_ERR_INVALID;
    if (m->extmap_count >= SDP_MAX_EXTMAP)
        return RTC_ERR_INVALID;
    /* If URI already present, just update id */
    for (int i = 0; i < m->extmap_count; i++) {
        if (strcmp(m->extmaps[i].uri, uri) == 0) {
            m->extmaps[i].id = id;
            return RTC_OK;
        }
    }
    m->extmaps[m->extmap_count].id = id;
    size_t ulen = strlen(uri);
    if (ulen >= sizeof(m->extmaps[m->extmap_count].uri))
        ulen = sizeof(m->extmaps[m->extmap_count].uri) - 1;
    memcpy(m->extmaps[m->extmap_count].uri, uri, ulen);
    m->extmaps[m->extmap_count].uri[ulen] = '\0';
    m->extmap_count++;
    return RTC_OK;
}

uint8_t rtc_sdp_media_find_extmap_id(const rtc_sdp_media_t *m, const char *uri) {
    if (!m || !uri)
        return 0;
    for (int i = 0; i < m->extmap_count; i++) {
        if (strcmp(m->extmaps[i].uri, uri) == 0)
            return m->extmaps[i].id;
    }
    return 0;
}
