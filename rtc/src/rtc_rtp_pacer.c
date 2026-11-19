#include "rtc_rtp_pacer.h"

#include <stdlib.h>
#include <string.h>

typedef struct rtc_pacer_packet {
    struct rtc_pacer_packet *next;
    size_t len, payload_len;
    uint8_t wire[];
} rtc_pacer_packet_t;

struct rtc_pacer_frame {
    rtc_pacer_frame_t *next;
    rtc_pacer_source_t *source;
    rtc_pacer_packet_t *head, *tail;
    uint32_t timestamp;
    uint64_t created_ms, ready_ms;
    size_t bytes;
};

struct rtc_pacer_source {
    rtc_pacer_source_t *next;
    uint32_t ssrc, timestamp;
    bool have_timestamp, failed, complete;
    rtc_pacer_frame_t *stage;
    rtc_pacer_sent_t sent;
    struct {
        uint16_t seq;
        uint32_t timestamp;
        uint64_t time_ms;
        bool valid;
    } history[128];
};

static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint16_t sequence(const uint8_t *wire) {
    return (uint16_t)((uint16_t)wire[2] << 8 | wire[3]);
}

static rtc_pacer_source_t *source(const rtc_rtp_pacer_t *p, uint32_t ssrc) {
    for (rtc_pacer_source_t *s = p->sources; s; s = s->next)
        if (s->ssrc == ssrc)
            return s;
    return NULL;
}

static void free_frame(rtc_rtp_pacer_t *p, rtc_pacer_frame_t *f, bool dropped) {
    if (dropped) {
        p->frames_dropped++;
        for (size_t i = 0; i < 128; i++)
            if (f->source->history[i].timestamp == f->timestamp)
                f->source->history[i].valid = false;
        if (f->source->timestamp == f->timestamp)
            f->source->failed = true;
    }
    while (f->head) {
        rtc_pacer_packet_t *packet = f->head;
        f->head = packet->next;
        p->bytes -= packet->len;
        p->packets--;
        f->source->sent.packets_queued--;
        if (dropped) {
            p->packets_dropped++;
            f->source->sent.packets_dropped++;
        }
        free(packet);
    }
    free(f);
}

static void replenish(rtc_rtp_pacer_t *p, uint64_t now) {
    /* Credit is in bit-milliseconds: 2.5 times the media target, capped at two MTUs. */
    const uint64_t cap = RTC_PACER_BURST_BYTES * 8000ULL;
    if (!p->clock_valid) {
        p->credit = cap;
        p->clock_valid = true;
    } else {
        uint64_t elapsed = now > p->last_ms ? now - p->last_ms : 0;
        uint64_t rate = (p->bitrate_bps ? p->bitrate_bps : 1500000ULL) * 5 / 2;
        if (elapsed > 1000)
            elapsed = 1000;
        p->credit += elapsed * rate;
        if (p->credit > cap)
            p->credit = cap;
    }
    p->last_ms = now;
}

int rtc_rtp_pacer_enqueue(rtc_rtp_pacer_t *p, const uint8_t *wire, size_t len, size_t payload_len,
                          uint64_t now) {
    if (!p || !wire || len < 12 || len > RTC_PACER_BURST_BYTES)
        return RTC_ERR_INVALID;
    uint32_t ssrc = u32(wire + 8), timestamp = u32(wire + 4);
    rtc_pacer_source_t *s = source(p, ssrc);
    if (!s) {
        if (p->source_count >= RTC_PACER_MAX_SOURCES || !(s = calloc(1, sizeof(*s)))) {
            p->packets_dropped++;
            return RTC_ERR_NOMEM;
        }
        s->ssrc = ssrc;
        s->next = p->sources;
        p->sources = s;
        p->source_count++;
    }
    if (s->have_timestamp && (int32_t)(timestamp - s->timestamp) < 0)
        goto rejected;
    if (!s->have_timestamp || timestamp != s->timestamp) {
        if (s->stage) {
            free_frame(p, s->stage, true);
            s->stage = NULL;
        }
        s->timestamp = timestamp;
        s->have_timestamp = true;
        s->failed = s->complete = false;
    }
    if (s->failed || s->complete)
        goto rejected;
    if (!s->stage) {
        s->stage = calloc(1, sizeof(*s->stage));
        if (!s->stage)
            goto failed;
        s->stage->source = s;
        s->stage->timestamp = timestamp;
        s->stage->created_ms = now;
    }
    rtc_pacer_frame_t *f = s->stage;
    if (now - f->created_ms >= RTC_PACER_STAGE_AGE_MS ||
        len > RTC_PACER_MAX_FRAME_BYTES - f->bytes || len > RTC_PACER_MAX_BYTES - p->bytes ||
        p->packets >= 1024)
        goto failed;
    rtc_pacer_packet_t *packet = malloc(sizeof(*packet) + len);
    if (!packet)
        goto failed;
    *packet = (rtc_pacer_packet_t){.len = len, .payload_len = payload_len};
    memcpy(packet->wire, wire, len);
    if (f->tail)
        f->tail->next = packet;
    else
        f->head = packet;
    f->tail = packet;
    f->bytes += len;
    p->bytes += len;
    p->packets++;
    s->sent.packets_queued++;
    if (wire[1] & 0x80) {
        f->ready_ms = now;
        s->stage = NULL;
        s->complete = true;
        if (p->tail)
            p->tail->next = f;
        else
            p->head = f;
        p->tail = f;
    }
    return RTC_OK;
failed:
    if (s->stage) {
        free_frame(p, s->stage, true);
        s->stage = NULL;
    }
    s->failed = true;
rejected:
    p->packets_dropped++;
    s->sent.packets_dropped++;
    s->sent.packets_rejected++;
    return RTC_ERR_NOMEM;
}

void rtc_rtp_pacer_run(rtc_rtp_pacer_t *p, uint64_t now, rtc_pacer_write_fn write, void *user) {
    replenish(p, now);
    for (rtc_pacer_source_t *s = p->sources; s; s = s->next)
        if (s->stage && now - s->stage->created_ms >= RTC_PACER_STAGE_AGE_MS) {
            free_frame(p, s->stage, true);
            s->stage = NULL;
        }
    while (p->head) {
        rtc_pacer_frame_t *f = p->head;
        if (now - f->ready_ms >= RTC_PACER_MAX_AGE_MS) {
            p->head = f->next;
            free_frame(p, f, true);
            continue;
        }
        rtc_pacer_packet_t *packet = f->head;
        uint64_t cost = packet->len * 8000ULL;
        if (p->credit < cost)
            break;
        p->credit -= cost;
        if (write(packet->wire, packet->len, user) != RTC_OK) {
            p->head = f->next;
            free_frame(p, f, true);
            continue;
        }
        rtc_pacer_source_t *s = f->source;
        uint16_t seq = sequence(packet->wire);
        size_t slot = seq % 128;
        s->history[slot].seq = seq;
        s->history[slot].timestamp = f->timestamp;
        s->history[slot].time_ms = now;
        s->history[slot].valid = true;
        s->sent.packets_sent++;
        s->sent.octets_sent += packet->payload_len;
        s->sent.timestamp = f->timestamp;
        s->sent.send_time_ms = f->created_ms;
        p->packets_sent++;
        p->bytes_sent += packet->len;
        p->bytes -= packet->len;
        p->packets--;
        s->sent.packets_queued--;
        f->head = packet->next;
        free(packet);
        if (!f->head) {
            p->head = f->next;
            free(f);
        }
    }
    if (!p->head)
        p->tail = NULL;
}

bool rtc_rtp_pacer_pending(const rtc_rtp_pacer_t *p) {
    return p->packets != 0;
}

void rtc_rtp_pacer_discard(rtc_rtp_pacer_t *p, uint32_t ssrc) {
    rtc_pacer_source_t **slot = &p->sources;
    while (*slot && (*slot)->ssrc != ssrc)
        slot = &(*slot)->next;
    rtc_pacer_source_t *s = *slot;
    if (!s)
        return;
    if (s->stage)
        free_frame(p, s->stage, true);
    s->stage = NULL;
    rtc_pacer_frame_t **link = &p->head;
    p->tail = NULL;
    while (*link) {
        rtc_pacer_frame_t *f = *link;
        if (f->source == s) {
            *link = f->next;
            free_frame(p, f, true);
        } else {
            p->tail = f;
            link = &f->next;
        }
    }
    memset(s->history, 0, sizeof(s->history));
    s->failed = true;
}

void rtc_rtp_pacer_forget(rtc_rtp_pacer_t *p, uint32_t ssrc) {
    rtc_rtp_pacer_discard(p, ssrc);
    rtc_pacer_source_t **slot = &p->sources;
    while (*slot && (*slot)->ssrc != ssrc)
        slot = &(*slot)->next;
    rtc_pacer_source_t *s = *slot;
    if (!s)
        return;
    *slot = s->next;
    p->source_count--;
    free(s);
}

void rtc_rtp_pacer_close(rtc_rtp_pacer_t *p) {
    while (p->sources)
        rtc_rtp_pacer_forget(p, p->sources->ssrc);
}

void rtc_rtp_pacer_clear(rtc_rtp_pacer_t *p) {
    for (rtc_pacer_source_t *s = p->sources; s; s = s->next)
        rtc_rtp_pacer_discard(p, s->ssrc);
}

bool rtc_rtp_pacer_sent(const rtc_rtp_pacer_t *p, uint32_t ssrc, rtc_pacer_sent_t *out) {
    rtc_pacer_source_t *s = source(p, ssrc);
    if (!s)
        return false;
    *out = s->sent;
    return true;
}

bool rtc_rtp_pacer_repair(rtc_rtp_pacer_t *p, const uint8_t *wire, size_t len, uint64_t now) {
    if (len < 12)
        return false;
    rtc_pacer_source_t *s = source(p, u32(wire + 8));
    if (!s)
        return false;
    uint16_t seq = sequence(wire);
    size_t slot = seq % 128;
    if (!s->history[slot].valid || s->history[slot].seq != seq ||
        s->history[slot].timestamp != u32(wire + 4) ||
        now - s->history[slot].time_ms >= RTC_PACER_MAX_AGE_MS)
        return false;
    replenish(p, now);
    if (p->credit < len * 8000ULL)
        return false;
    p->credit -= len * 8000ULL;
    return true;
}
