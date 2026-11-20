#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include <rtc/rtc.h>
#include <rtc/rtc_listener.h>
#include <rtc/rtc_transport.h>

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#define PAYLOAD_BYTES 1000
#define WINDOW        32
#define MAGIC         UINT64_C(0x4d52544342454e43)

typedef struct {
    rtc_worker_t *worker;
    rtc_listener_t *listener;
    rtc_transport_t *transport;
} endpoint_t;

typedef struct {
    _Atomic uint64_t received;
    _Atomic bool invalid;
    _Atomic uint64_t phase;
    size_t capacity;
    uint8_t *seen;
    uint64_t *latencies;
} receiver_t;

typedef struct {
    uint64_t packets;
    double seconds;
    double cpu_percent;
    double p50_ms;
    double p95_ms;
    double max_ms;
} measurement_t;

static uint64_t now_ns(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        abort();
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}

static void pause_ns(long ns) {
    struct timespec value = {.tv_nsec = ns};
    while (nanosleep(&value, &value) != 0 && errno == EINTR) {}
}

static uint64_t get_u64(const uint8_t *bytes) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; i++)
        value = (value << 8) | bytes[i];
    return value;
}

static void put_u64(uint8_t *bytes, uint64_t value) {
    for (unsigned i = 0; i < 8; i++)
        bytes[i] = (uint8_t)(value >> (56 - i * 8));
}

static void received_rtp(const rtc_rtp_packet_t *packet, void *user) {
    receiver_t *receiver = user;
    uint64_t arrival = now_ns();
    if (packet->header.ssrc != 0x11223344 || packet->header.payload_type != 96 ||
        packet->payload_len != PAYLOAD_BYTES || get_u64(packet->payload) != MAGIC ||
        get_u64(packet->payload + 8) != atomic_load(&receiver->phase)) {
        atomic_store(&receiver->invalid, true);
        return;
    }
    uint64_t index = get_u64(packet->payload + 16);
    uint64_t sent = get_u64(packet->payload + 24);
    if (index >= receiver->capacity || receiver->seen[index] || arrival <= sent) {
        atomic_store(&receiver->invalid, true);
        return;
    }
    for (size_t i = 32; i < PAYLOAD_BYTES; i++) {
        if (packet->payload[i] != (uint8_t)(index + i)) {
            atomic_store(&receiver->invalid, true);
            return;
        }
    }
    receiver->seen[index] = 1;
    receiver->latencies[index] = arrival - sent;
    atomic_fetch_add_explicit(&receiver->received, 1, memory_order_release);
}

static bool endpoint_create(endpoint_t *endpoint) {
    endpoint->worker = rtc_worker_create(NULL);
    if (!endpoint->worker)
        return false;
    rtc_listener_config_t listener_config = {.listen_ip = "127.0.0.1", .enable_udp = true};
    endpoint->listener = rtc_listener_create(endpoint->worker, &listener_config);
    if (!endpoint->listener)
        return false;
    rtc_transport_config_t config = {
        .listener = endpoint->listener, .ice_mode = RTC_ICE_MODE_FULL, .consent_timeout_ms = 30000};
    endpoint->transport = rtc_transport_create(endpoint->worker, &config);
    return endpoint->transport != NULL;
}

static void endpoint_close(endpoint_t *endpoint) {
    if (endpoint->transport) {
        rtc_transport_close(endpoint->transport);
        rtc_transport_destroy(endpoint->transport);
    }
    if (endpoint->listener) {
        rtc_listener_close(endpoint->listener);
        rtc_listener_destroy(endpoint->listener);
    }
    if (endpoint->worker) {
        rtc_worker_close(endpoint->worker);
        rtc_worker_destroy(endpoint->worker);
    }
}

static bool exchange(endpoint_t *a, endpoint_t *b, bool bad_fingerprint) {
    rtc_ice_parameters_t ai, bi;
    rtc_dtls_parameters_t ad, bd;
    rtc_addr_t aa, ba;
    char ip[64];
    uint16_t ap, bp;
    if (rtc_transport_get_ice_parameters(a->transport, &ai) != RTC_OK ||
        rtc_transport_get_ice_parameters(b->transport, &bi) != RTC_OK ||
        rtc_listener_get_local_addr(a->listener, &aa) != RTC_OK ||
        rtc_listener_get_local_addr(b->listener, &ba) != RTC_OK ||
        rtc_addr_to_string(&aa, ip, sizeof(ip), &ap) != RTC_OK ||
        rtc_addr_to_string(&ba, ip, sizeof(ip), &bp) != RTC_OK ||
        rtc_transport_set_ice_controlling(a->transport, true) != RTC_OK ||
        rtc_transport_set_ice_controlling(b->transport, false) != RTC_OK ||
        rtc_transport_set_dtls_role(b->transport, RTC_TRANSPORT_DTLS_ROLE_SERVER) != RTC_OK ||
        rtc_transport_get_dtls_parameters(a->transport, &ad) != RTC_OK ||
        rtc_transport_get_dtls_parameters(b->transport, &bd) != RTC_OK)
        return false;
    if (!ad.fingerprint[0] || !bd.fingerprint[0])
        return false;
    if (bad_fingerprint)
        bd.fingerprint[0] = bd.fingerprint[0] == '0' ? '1' : '0';
    rtc_transport_candidate_t ac = {.address = "127.0.0.1", .protocol = "udp", .port = ap};
    rtc_transport_candidate_t bc = {.address = "127.0.0.1", .protocol = "udp", .port = bp};
    return rtc_transport_set_remote_dtls_parameters(a->transport, &bd) == RTC_OK &&
           rtc_transport_set_remote_dtls_parameters(b->transport, &ad) == RTC_OK &&
           rtc_transport_set_remote_ice_parameters(a->transport, &bi) == RTC_OK &&
           rtc_transport_set_remote_ice_parameters(b->transport, &ai) == RTC_OK &&
           rtc_transport_add_remote_candidate(a->transport, &bc) == RTC_OK &&
           rtc_transport_add_remote_candidate(b->transport, &ac) == RTC_OK &&
           rtc_transport_start_ice(a->transport) == RTC_OK &&
           rtc_transport_start_ice(b->transport) == RTC_OK;
}

static bool wait_ready(endpoint_t *a, endpoint_t *b) {
    uint64_t deadline = now_ns() + UINT64_C(5000000000);
    bool started = false;
    while (now_ns() < deadline) {
        rtc_transport_stats_t as, bs;
        if (rtc_transport_get_stats(a->transport, &as) != RTC_OK ||
            rtc_transport_get_stats(b->transport, &bs) != RTC_OK ||
            as.dtls_state == RTC_TRANSPORT_DTLS_FAILED ||
            bs.dtls_state == RTC_TRANSPORT_DTLS_FAILED)
            return false;
        if (as.selected_tuple_valid && bs.selected_tuple_valid && !started) {
            if (rtc_transport_start_dtls(a->transport) != RTC_OK)
                return false;
            started = true;
        }
        if (as.dtls_state == RTC_TRANSPORT_DTLS_CONNECTED &&
            bs.dtls_state == RTC_TRANSPORT_DTLS_CONNECTED && as.srtp_ready && bs.srtp_ready)
            return true;
        pause_ns(1000000);
    }
    return false;
}

static double cpu_seconds(const struct rusage *usage) {
    return (double)usage->ru_utime.tv_sec + (double)usage->ru_stime.tv_sec +
           ((double)usage->ru_utime.tv_usec + (double)usage->ru_stime.tv_usec) / 1000000;
}

static int compare_latency(const void *a, const void *b) {
    uint64_t left = *(const uint64_t *)a, right = *(const uint64_t *)b;
    return (left > right) - (left < right);
}

static bool phase(endpoint_t *sender, receiver_t *receiver, uint64_t phase_id, unsigned seconds,
                  bool smoke, uint64_t *sequence, measurement_t *out) {
    memset(receiver->seen, 0, receiver->capacity);
    atomic_store(&receiver->received, 0);
    atomic_store(&receiver->phase, phase_id);
    struct rusage before, after;
    if (getrusage(RUSAGE_SELF, &before) != 0)
        return false;
    uint64_t started = now_ns(), sent = 0;
    uint64_t deadline = started + (uint64_t)seconds * UINT64_C(1000000000);
    do {
        if (sent + WINDOW > receiver->capacity) {
            fprintf(stderr, "Packet cap reached before duration; no valid measurement\n");
            return false;
        }
        for (unsigned i = 0; i < WINDOW; i++) {
            uint8_t payload[PAYLOAD_BYTES];
            put_u64(payload, MAGIC);
            put_u64(payload + 8, phase_id);
            put_u64(payload + 16, sent);
            for (size_t j = 32; j < sizeof(payload); j++)
                payload[j] = (uint8_t)(sent + j);
            put_u64(payload + 24, now_ns());
            rtc_rtp_packet_t packet;
            if (rtc_rtp_build(&packet, 96, (uint16_t)*sequence, (uint32_t)*sequence, 0x11223344,
                              false, payload, sizeof(payload)) != RTC_OK)
                return false;
            size_t length = packet.buf_len;
            if (rtc_transport_send_rtp(sender->transport, packet.buf, &length,
                                       sizeof(packet.buf)) != RTC_OK)
                return false;
            (*sequence)++;
            sent++;
        }
        uint64_t drain_deadline = now_ns() + UINT64_C(3000000000);
        while (atomic_load_explicit(&receiver->received, memory_order_acquire) < sent) {
            if (atomic_load(&receiver->invalid) || now_ns() >= drain_deadline) {
                fprintf(stderr, "Missing, duplicate, corrupt or late SRTP payload\n");
                return false;
            }
            // This bounded driver wait is included in achieved throughput and CPU.
            pause_ns(100000);
        }
    } while (!smoke && now_ns() < deadline);
    uint64_t ended = now_ns();
    if (getrusage(RUSAGE_SELF, &after) != 0 || atomic_load(&receiver->invalid) ||
        atomic_load(&receiver->received) != sent)
        return false;
    for (uint64_t i = 0; i < sent; i++) {
        if (receiver->seen[i] != 1)
            return false;
    }
    qsort(receiver->latencies, (size_t)sent, sizeof(*receiver->latencies), compare_latency);
    out->packets = sent;
    out->seconds = (double)(ended - started) / 1000000000;
    out->cpu_percent = (cpu_seconds(&after) - cpu_seconds(&before)) / out->seconds * 100;
    out->p50_ms = (double)receiver->latencies[(sent - 1) / 2] / 1000000;
    out->p95_ms = (double)receiver->latencies[(sent * 95 + 99) / 100 - 1] / 1000000;
    out->max_ms = (double)receiver->latencies[sent - 1] / 1000000;
    return true;
}

static bool parse_number(const char *text, unsigned min, unsigned max, unsigned *out) {
    char *end;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (!text[0] || text[0] == '-' || *end || errno || value < min || value > max)
        return false;
    *out = (unsigned)value;
    return true;
}

int main(int argc, char **argv) {
    bool smoke = argc == 2 && strcmp(argv[1], "--smoke") == 0;
    bool bad_fingerprint = argc == 2 && strcmp(argv[1], "--bad-fingerprint") == 0;
    unsigned seconds = 15, warmup = 5, capacity = 2000000;
    if (!smoke && !bad_fingerprint &&
        (argc != 4 || !parse_number(argv[1], 1, 60, &seconds) ||
         !parse_number(argv[2], 1, 60, &warmup) ||
         !parse_number(argv[3], 1024, 8000000, &capacity))) {
        fprintf(stderr, "Usage: benchmark-transport seconds warmup packet-cap | --smoke | "
                        "--bad-fingerprint\n");
        return 2;
    }
    if (smoke || bad_fingerprint)
        capacity = 1024;
    endpoint_t a = {0}, b = {0};
    receiver_t receiver = {.capacity = capacity};
    measurement_t ignored = {0}, measured = {0};
    bool initialized = rtc_init() == RTC_OK;
    bool ok = false;
    const char *error = "initialization or allocation failed";
    receiver.seen = calloc(capacity, sizeof(*receiver.seen));
    receiver.latencies = calloc(capacity, sizeof(*receiver.latencies));
    if (!initialized || !receiver.seen || !receiver.latencies || !endpoint_create(&a) ||
        !endpoint_create(&b))
        goto done;
    rtc_transport_on_rtp(b.transport, received_rtp, &receiver);
    error = "ICE/fingerprint-verified DTLS/SRTP setup failed";
    if (!exchange(&a, &b, bad_fingerprint) || !wait_ready(&a, &b))
        goto done;
    // Fail even if a broken runtime accepts the deliberately incorrect certificate pin.
    if (bad_fingerprint) {
        error = "incorrect fingerprint was accepted";
        goto done;
    }
    uint64_t sequence = 1;
    error = "warmup packet verification failed";
    if (!phase(&a, &receiver, 1, warmup, smoke, &sequence, &ignored))
        goto done;
    error = "measurement packet verification/cap/deadline failed";
    if (!phase(&a, &receiver, 2, seconds, smoke, &sequence, &measured))
        goto done;
    ok = true;
done:
    endpoint_close(&a);
    endpoint_close(&b);
    if (atomic_load(&receiver.invalid)) {
        ok = false;
        error = "unexpected/duplicate payload observed through shutdown";
    }
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        ok = false;
        error = "resource accounting failed";
    }
    if (ok) {
#ifdef __APPLE__
        uint64_t rss = (uint64_t)usage.ru_maxrss;
#else
        uint64_t rss = (uint64_t)usage.ru_maxrss * 1024;
#endif
        printf("{\"type\":\"transport-harness\",\"ok\":true,\"fingerprintVerified\":true,"
               "\"smoke\":%s,\"packets\":%llu,\"elapsedSeconds\":%.9f,"
               "\"metrics\":{\"transportCpuPercent\":%.9f,\"transportPeakRssBytes\":%llu,"
               "\"transportDeliveredPacketsPerSecond\":%.9f,"
               "\"transportPayloadBytesPerSecond\":%.9f,\"transportOneWayP50Ms\":%.9f,"
               "\"transportOneWayP95Ms\":%.9f,\"transportOneWayMaxMs\":%.9f}}\n",
               smoke ? "true" : "false", (unsigned long long)measured.packets, measured.seconds,
               measured.cpu_percent, (unsigned long long)rss,
               (double)measured.packets / measured.seconds,
               (double)measured.packets * PAYLOAD_BYTES / measured.seconds, measured.p50_ms,
               measured.p95_ms, measured.max_ms);
    } else {
        printf("{\"type\":\"transport-harness\",\"ok\":false,\"error\":\"%s\"}\n", error);
    }
    free(receiver.seen);
    free(receiver.latencies);
    if (initialized)
        rtc_cleanup();
    return ok ? 0 : 1;
}
