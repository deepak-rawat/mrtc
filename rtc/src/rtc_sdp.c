/*
 * SDP (RFC 4566) generation and parsing for WebRTC.
 *
 * Supports multiple m= lines: audio, video, and application (data channel).
 */
#include "rtc/rtc_sdp.h"
#include "rtc/rtc_rtcp.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

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
        SDP_WRITE(p, remain, "m=audio %d UDP/TLS/RTP/SAVPF %d\r\n", m->rejected ? 0 : 9,
                  m->payload_type);
    } else if (m->media_type == RTC_MEDIA_VIDEO) {
        SDP_WRITE(p, remain, "m=video %d UDP/TLS/RTP/SAVPF %d\r\n", m->rejected ? 0 : 9,
                  m->payload_type);
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
        static const char *directions[] = {"sendrecv", "sendonly", "recvonly", "inactive"};
        if (m->direction < 0 || m->direction > 3)
            return RTC_ERR_INVALID;
        SDP_WRITE(p, remain, "a=%s\r\n", directions[m->direction]);
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
        static const struct {
            uint8_t bit;
            const char *name;
        } feedback[] = {{RTC_SDP_FB_NACK, "nack"},
                        {RTC_SDP_FB_PLI, "nack pli"},
                        {RTC_SDP_FB_FIR, "ccm fir"},
                        {RTC_SDP_FB_TRANSPORT_CC, "transport-cc"},
                        {RTC_SDP_FB_REMB, "goog-remb"}};
        for (size_t i = 0; i < sizeof(feedback) / sizeof(feedback[0]); i++)
            if (m->rtcp_feedback & feedback[i].bit)
                SDP_WRITE(p, remain, "a=rtcp-fb:%d %s\r\n", m->payload_type, feedback[i].name);
        for (int i = 0; i < m->extmap_count; i++) {
            if (m->extmaps[i].id == 0 || m->extmaps[i].uri[0] == '\0')
                continue;
            SDP_WRITE(p, remain, "a=extmap:%u %s\r\n", (unsigned)m->extmaps[i].id,
                      m->extmaps[i].uri);
        }
    }

    /* Data channel attributes */
    if (m->media_type == RTC_MEDIA_APPLICATION) {
        SDP_WRITE(p, remain, "a=sctp-port:%u\r\n", m->sctp_port ? m->sctp_port : 5000);
        SDP_WRITE(p, remain, "a=max-message-size:%u\r\n",
                  m->max_message_size ? m->max_message_size : 65536);
    }

    if ((m->media_type == RTC_MEDIA_AUDIO || m->media_type == RTC_MEDIA_VIDEO) && m->ssrc != 0) {
        const char *cname = rtc_rtcp_cname();
        if (!cname)
            return RTC_ERR_SSL;
        SDP_WRITE(p, remain, "a=ssrc:%u cname:%s\r\n", m->ssrc, cname);
    }

    return RTC_OK;
}

int rtc_sdp_generate(rtc_sdp_t *sdp) {
    if (!sdp || sdp->media_count < 0 || sdp->media_count > SDP_MAX_MEDIA)
        return RTC_ERR_INVALID;
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
    if (sdp->ice_lite)
        SDP_WRITE(&p, &remain, "a=ice-lite\r\n");

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

    for (int i = 0; i < effective_media_count; i++) {
        int rc = sdp_write_media_section(&p, &remain, &effective_media[i], sdp);
        if (rc != RTC_OK)
            return rc;
        if (i != 0)
            continue;
        /* The first bundled m-section owns the shared ICE transport. */
        size_t ncand = rtc_vec_len(&sdp->candidates);
        for (size_t j = 0; j < ncand; j++) {
            const rtc_ice_candidate_t *c =
                (const rtc_ice_candidate_t *)rtc_vec_at(&sdp->candidates, j);
            char cand_line[256];
            if (rtc_ice_candidate_to_string(c, cand_line, sizeof(cand_line)) != RTC_OK)
                return RTC_ERR_INVALID;
            SDP_WRITE(&p, &remain, "a=%s\r\n", cand_line);
        }
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

bool rtc_ice_candidate_is_mdns_hostname(const char *hostname) {
    if (!hostname)
        return false;
    size_t len = strlen(hostname);
    if (len && hostname[len - 1] == '.')
        len--;
    if (len <= 6 || len > 253)
        return false;
    const char *suffix = ".local";
    for (size_t i = 0; i < 6; i++)
        if (tolower((unsigned char)hostname[len - 6 + i]) != suffix[i])
            return false;
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || hostname[i] == '.') {
            if (i == start || i - start > 63 || hostname[start] == '-' || hostname[i - 1] == '-')
                return false;
            start = i + 1;
        } else {
            unsigned char ch = (unsigned char)hostname[i];
            if (ch >= 128 || (!isalnum(ch) && ch != '-'))
                return false;
        }
    }
    return true;
}

static int candidate_token(const char **cursor, char *out, size_t capacity) {
    const char *p = *cursor;
    while (*p == ' ' || *p == '\t')
        p++;
    if (!*p || *p == '\r' || *p == '\n') {
        *cursor = p;
        return 0;
    }
    size_t len = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
        if ((unsigned char)*p < 33 || (unsigned char)*p > 126 || len + 1 >= capacity)
            return -1;
        out[len++] = *p++;
    }
    out[len] = '\0';
    *cursor = p;
    return 1;
}

static bool candidate_uint(const char *text, uint32_t maximum, uint32_t *out) {
    if (text[0] < '0' || text[0] > '9')
        return false;
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno == ERANGE || !end || *end || value > maximum)
        return false;
    *out = (uint32_t)value;
    return true;
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

    rtc_ice_candidate_t c = {0};
    char component[12], priority[12], port_text[12], tag[4], type_str[8], address[256];
    struct {
        char *text;
        size_t capacity;
    } fields[] = {{c.foundation, sizeof(c.foundation)},
                  {component, sizeof(component)},
                  {c.protocol, sizeof(c.protocol)},
                  {priority, sizeof(priority)},
                  {address, sizeof(address)},
                  {port_text, sizeof(port_text)},
                  {tag, sizeof(tag)},
                  {type_str, sizeof(type_str)}};
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
        if (candidate_token(&p, fields[i].text, fields[i].capacity) != 1)
            return RTC_ERR_SDP;
    uint32_t component_value, port;
    if (!candidate_uint(component, 256, &component_value) || !component_value ||
        !candidate_uint(priority, INT32_MAX, &c.priority) || !c.priority ||
        !candidate_uint(port_text, UINT16_MAX, &port) || !port || strcmp(tag, "typ"))
        return RTC_ERR_SDP;
    c.component = (int)component_value;
    for (size_t i = 0; c.protocol[i]; i++)
        c.protocol[i] = (char)tolower((unsigned char)c.protocol[i]);
    if (strcmp(c.protocol, "udp") && strcmp(c.protocol, "tcp"))
        return RTC_ERR_SDP;

    if (strcmp(type_str, "host") == 0)
        c.type = ICE_CANDIDATE_HOST;
    else if (strcmp(type_str, "srflx") == 0)
        c.type = ICE_CANDIDATE_SRFLX;
    else if (strcmp(type_str, "relay") == 0)
        c.type = ICE_CANDIDATE_RELAY;
    else if (strcmp(type_str, "prflx") == 0)
        c.type = ICE_CANDIDATE_PRFLX;
    else
        return RTC_ERR_SDP;

    if (rtc_addr_from_string(&c.addr, address, (uint16_t)port) != RTC_OK) {
        if (c.type != ICE_CANDIDATE_HOST || !rtc_ice_candidate_is_mdns_hostname(address))
            return RTC_ERR_SDP;
        memcpy(c.hostname, address, strlen(address) + 1);
        c.hostname_port = (uint16_t)port;
    }
    char key[64], value[256], related_ip[64] = {0};
    uint32_t related_port = 0;
    bool has_raddr = false, has_rport = false, has_tcp = false, has_ufrag = false;
    int token;
    while ((token = candidate_token(&p, key, sizeof(key))) == 1) {
        if (candidate_token(&p, value, sizeof(value)) != 1)
            return RTC_ERR_SDP;
        if (strcmp(key, "raddr") == 0) {
            if (has_raddr || strlen(value) >= sizeof(related_ip))
                return RTC_ERR_SDP;
            memcpy(related_ip, value, strlen(value) + 1);
            has_raddr = true;
        } else if (strcmp(key, "rport") == 0) {
            if (has_rport || !candidate_uint(value, UINT16_MAX, &related_port))
                return RTC_ERR_SDP;
            has_rport = true;
        } else if (strcmp(key, "tcptype") == 0) {
            if (has_tcp || strcmp(c.protocol, "tcp") ||
                (strcmp(value, "active") && strcmp(value, "passive") && strcmp(value, "so")))
                return RTC_ERR_SDP;
            memcpy(c.tcp_type, value, strlen(value) + 1);
            has_tcp = true;
        } else if (strcmp(key, "ufrag") == 0) {
            if (has_ufrag || strlen(value) >= sizeof(c.username_fragment))
                return RTC_ERR_SDP;
            memcpy(c.username_fragment, value, strlen(value) + 1);
            has_ufrag = true;
        }
    }
    if (token < 0 || (strcmp(c.protocol, "tcp") == 0 && !has_tcp) || has_raddr != has_rport)
        return RTC_ERR_SDP;
    if (has_raddr) {
        if (rtc_addr_from_string(&c.related_addr, related_ip, (uint16_t)related_port) != RTC_OK)
            return RTC_ERR_SDP;
        if (!related_port && strcmp(related_ip, "0.0.0.0") && strcmp(related_ip, "::"))
            return RTC_ERR_SDP;
        c.has_related_addr = related_port != 0;
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
        case ICE_CANDIDATE_PRFLX:
            type_pref = 110;
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

    char ip[256];
    uint16_t port = 0;
    if (candidate->hostname[0]) {
        if (candidate->type != ICE_CANDIDATE_HOST || !candidate->hostname_port ||
            !rtc_ice_candidate_is_mdns_hostname(candidate->hostname))
            return RTC_ERR_INVALID;
        memcpy(ip, candidate->hostname, strlen(candidate->hostname) + 1);
        port = candidate->hostname_port;
    } else {
        int rc = rtc_addr_to_string(&candidate->addr, ip, sizeof(ip), &port);
        if (rc != RTC_OK)
            return rc;
    }

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
        case ICE_CANDIDATE_PRFLX:
            type_str = "prflx";
            break;
        default:
            return RTC_ERR_INVALID;
    }

    const char *protocol = candidate->protocol[0] ? candidate->protocol : "udp";
    if (strcmp(protocol, "udp") && strcmp(protocol, "tcp"))
        return RTC_ERR_INVALID;
    if (strcmp(protocol, "tcp") == 0 && strcmp(candidate->tcp_type, "active") &&
        strcmp(candidate->tcp_type, "passive") && strcmp(candidate->tcp_type, "so"))
        return RTC_ERR_INVALID;
    int n = snprintf(buf, len, "candidate:%s %d %s %u %s %u typ %s", candidate->foundation,
                     candidate->component, protocol, candidate->priority, ip, port, type_str);
    if (n < 0 || (size_t)n >= len)
        return RTC_ERR_NOMEM;
    size_t used = (size_t)n;
    if (strcmp(protocol, "tcp") == 0) {
        n = snprintf(buf + used, len - used, " tcptype %s", candidate->tcp_type);
        if (n < 0 || (size_t)n >= len - used)
            return RTC_ERR_NOMEM;
        used += (size_t)n;
    }
    if (candidate->has_related_addr) {
        char related_ip[64];
        uint16_t related_port = 0;
        int rc = rtc_addr_to_string(&candidate->related_addr, related_ip, sizeof(related_ip),
                                    &related_port);
        if (rc != RTC_OK)
            return rc;
        int rn = snprintf(buf + used, len - used, " raddr %s rport %u", related_ip, related_port);
        if (rn < 0 || (size_t)rn >= len - used)
            return RTC_ERR_NOMEM;
        used += (size_t)rn;
    }
    if (candidate->username_fragment[0]) {
        n = snprintf(buf + used, len - used, " ufrag %s", candidate->username_fragment);
        if (n < 0 || (size_t)n >= len - used)
            return RTC_ERR_NOMEM;
    }

    return RTC_OK;
}

int rtc_sdp_parse(rtc_sdp_t *sdp, const char *text, size_t len) {
    if (!sdp || !text || len >= sizeof(sdp->raw))
        return RTC_ERR_INVALID;
    /* Store raw text */
    memmove(sdp->raw, text, len);
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

    const char *p = sdp->raw;
    const char *end = sdp->raw + len;

    /* Index of the current m= section being parsed (-1 = session level) */
    int current_media = -1;
    uint8_t feedback[SDP_MAX_MEDIA][128] = {{0}};
    uint8_t wildcard[SDP_MAX_MEDIA] = {0};

    while (p < end) {
        const char *line = p;
        const char *nl = p;
        while (nl < end && *nl != '\n' && *nl != '\r')
            nl++;
        size_t line_len = (size_t)(nl - line);
        if (line_len == 10 && memcmp(line, "a=ice-lite", 10) == 0)
            sdp->ice_lite = true;
        if (sdp_line_starts_n(line, line_len, "m=") && sdp->media_count == SDP_MAX_MEDIA)
            return RTC_ERR_NOMEM;

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
                int port;
                if (sscanf(line, "m=audio %d", &port) != 1 || port < 0 || port > 65535)
                    return RTC_ERR_SDP;
                m->rejected = port == 0;
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
                int port;
                if (sscanf(line, "m=video %d", &port) != 1 || port < 0 || port > 65535)
                    return RTC_ERR_SDP;
                m->rejected = port == 0;
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

        if (current_media >= 0 && current_media < sdp->media_count) {
            rtc_sdp_media_t *m = &sdp->media[current_media];
            if (line_len == 10 && !memcmp(line, "a=rtcp-mux", 10))
                m->rtcp_mux = true;
            if (sdp_line_starts_n(line, line_len, "a=rtcp-fb:")) {
                char copy[256], pt[8], kind[64];
                if (line_len >= sizeof(copy))
                    return RTC_ERR_SDP;
                memcpy(copy, line, line_len);
                copy[line_len] = '\0';
                if (sscanf(copy, "a=rtcp-fb:%7s %63[^\r\n]", pt, kind) != 2)
                    return RTC_ERR_SDP;
                uint8_t bit = !strcmp(kind, "nack")           ? RTC_SDP_FB_NACK
                              : !strcmp(kind, "nack pli")     ? RTC_SDP_FB_PLI
                              : !strcmp(kind, "ccm fir")      ? RTC_SDP_FB_FIR
                              : !strcmp(kind, "transport-cc") ? RTC_SDP_FB_TRANSPORT_CC
                              : !strcmp(kind, "goog-remb")    ? RTC_SDP_FB_REMB
                                                              : 0;
                uint32_t payload;
                if (!strcmp(pt, "*"))
                    wildcard[current_media] |= bit;
                else if (candidate_uint(pt, 127, &payload))
                    feedback[current_media][payload] |= bit;
                else
                    return RTC_ERR_SDP;
            }
            if (sdp_line_starts_n(line, line_len, "a=sendrecv"))
                m->direction = 0;
            else if (sdp_line_starts_n(line, line_len, "a=sendonly"))
                m->direction = 1;
            else if (sdp_line_starts_n(line, line_len, "a=recvonly"))
                m->direction = 2;
            else if (sdp_line_starts_n(line, line_len, "a=inactive"))
                m->direction = 3;
            else if (sdp_line_starts_n(line, line_len, "a=sctp-port:")) {
                unsigned port = 0;
                if (sscanf(line + 12, "%u", &port) != 1 || port == 0 || port > 65535)
                    return RTC_ERR_INVALID;
                m->sctp_port = (uint16_t)port;
            } else if (sdp_line_starts_n(line, line_len, "a=max-message-size:")) {
                unsigned size = 0;
                if (sscanf(line + 19, "%u", &size) != 1)
                    return RTC_ERR_INVALID;
                m->max_message_size = size;
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
            int rc = rtc_sdp_parse_candidate_line(line, &c);
            if (rc != RTC_OK)
                return rc;
            if (rtc_vec_push(&sdp->candidates, &c) != RTC_OK)
                return RTC_ERR_NOMEM;
        }

        p = sdp_next_line(p, end);
    }

    for (int i = 0; i < sdp->media_count; i++) {
        rtc_sdp_media_t *media = &sdp->media[i];
        if (media->rejected)
            media->direction = 3;
        if (media->payload_type >= 0 && media->payload_type < 128)
            media->rtcp_feedback = feedback[i][media->payload_type] | wildcard[i];
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
