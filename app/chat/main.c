/*
 * chat - WebRTC chat using signaling server + data channels.
 *
 * Connects to a signaling server, joins a meeting, automatically
 * establishes peer connections with all participants, and exchanges
 * text messages over data channels.
 *
 * How to run:
 * ----------
 *   1. Start the signaling server (from build/):
 *        ./signaling/signaling_server 8080
 *
 *   2. In terminal A:
 *        ./app/chat/chat --meeting test
 *
 *   3. In terminal B:
 *        ./app/chat/chat --meeting test
 *
 *   4. Type messages in either terminal — they appear in all others.
 *      Type "quit" to leave.
 *
 * Options:
 *   --meeting <name>    Meeting name to join (required)
 *   --server <url>      Signaling server (default: ws://127.0.0.1:8080)
 *   --stun <ip>         STUN server IP (default: stun.l.google.com)
 *
 * Multiple peers (up to 8) can join the same meeting. Each peer
 * creates a peer connection + data channel to every other peer.
 * The newly joining peer always creates the offer (avoids glare).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rtc/rtc.h>
#include <signaling/signaling_client.h>
#include <signaling/signaling_msg.h>

#ifdef _WIN32
#  include <windows.h>
#  include <conio.h>
#  define SLEEP_MS(ms) Sleep(ms)
#else
#  include <unistd.h>
#  include <sys/select.h>
#  define SLEEP_MS(ms) usleep((ms) * 1000)
#endif

/* ------------------------------------------------------------------ */
/*  Per-peer state                                                     */
/* ------------------------------------------------------------------ */
#define MAX_PEERS 8

typedef struct {
    char peer_id[SIG_MAX_PEER_ID];
    rtc_peer_connection_t *pc;
    rtc_data_channel_t *dc;
    bool dc_open;
    bool active;
} peer_state_t;

static peer_state_t g_peers[MAX_PEERS];
static int g_peer_count = 0;
static char g_my_id[SIG_MAX_PEER_ID];
static const char *g_stun_ip = "stun.l.google.com";
static volatile bool g_running = true;

/* ------------------------------------------------------------------ */
/*  Peer state management                                              */
/* ------------------------------------------------------------------ */
static peer_state_t *find_peer(const char *peer_id) {
    for (int i = 0; i < g_peer_count; i++) {
        if (g_peers[i].active && strcmp(g_peers[i].peer_id, peer_id) == 0)
            return &g_peers[i];
    }
    return NULL;
}

static peer_state_t *add_peer(const char *peer_id) {
    if (g_peer_count >= MAX_PEERS)
        return NULL;
    peer_state_t *ps = &g_peers[g_peer_count++];
    memset(ps, 0, sizeof(*ps));
    snprintf(ps->peer_id, sizeof(ps->peer_id), "%s", peer_id);
    ps->active = true;
    return ps;
}

static void remove_peer(const char *peer_id) {
    for (int i = 0; i < g_peer_count; i++) {
        if (g_peers[i].active && strcmp(g_peers[i].peer_id, peer_id) == 0) {
            if (g_peers[i].pc) {
                rtc_peer_connection_close(g_peers[i].pc);
                rtc_peer_connection_destroy(g_peers[i].pc);
            }
            g_peers[i].active = false;
            printf("  [%s left]\n", peer_id);
            fflush(stdout);
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Data channel callbacks                                             */
/* ------------------------------------------------------------------ */
static void on_dc_open(void *user) {
    peer_state_t *ps = (peer_state_t *)user;
    ps->dc_open = true;
    printf("  [data channel open with %s]\n", ps->peer_id);
    fflush(stdout);
}

static void on_dc_close(void *user) {
    peer_state_t *ps = (peer_state_t *)user;
    ps->dc_open = false;
}

static void on_dc_message(const uint8_t *data, size_t len, void *user) {
    peer_state_t *ps = (peer_state_t *)user;
    printf("  [%s] %.*s\n", ps->peer_id, (int)len, (const char *)data);
    fflush(stdout);
}

static void setup_dc_callbacks(peer_state_t *ps) {
    if (!ps->dc)
        return;
    rtc_data_channel_on_open(ps->dc, on_dc_open, ps);
    rtc_data_channel_on_close(ps->dc, on_dc_close, ps);
    rtc_data_channel_on_message(ps->dc, on_dc_message, ps);
}

/* ------------------------------------------------------------------ */
/*  Peer connection callbacks                                          */
/* ------------------------------------------------------------------ */
static void on_connection_state(rtc_connection_state_t state, void *user) {
    peer_state_t *ps = (peer_state_t *)user;
    if (state == RTC_CONNECTION_CONNECTED)
        printf("  [connected to %s]\n", ps->peer_id);
    else if (state == RTC_CONNECTION_FAILED)
        printf("  [connection to %s failed]\n", ps->peer_id);
    fflush(stdout);
}

static void on_data_channel(rtc_data_channel_t *channel, void *user) {
    peer_state_t *ps = (peer_state_t *)user;
    ps->dc = channel;
    setup_dc_callbacks(ps);
    /* Channel may already be open (answerer receives OPEN+ACK before callback) */
    if (rtc_data_channel_state(channel) == RTC_DC_OPEN) {
        ps->dc_open = true;
        printf("  [data channel open with %s]\n", ps->peer_id);
        fflush(stdout);
    }
}

/* ------------------------------------------------------------------ */
/*  Create a peer connection for a remote peer                         */
/* ------------------------------------------------------------------ */
static signaling_client_t *g_signaling; /* forward ref */

static rtc_peer_connection_t *create_pc(peer_state_t *ps) {
    rtc_config_t config;
    memset(&config, 0, sizeof(config));
    config.ice_servers[0].urls[0] = g_stun_ip;
    config.ice_servers[0].url_count = 1;
    config.ice_server_count = 1;

    rtc_peer_connection_t *pc = rtc_peer_connection_create(&config);
    if (!pc)
        return NULL;

    rtc_peer_connection_on_connection_state(pc, on_connection_state, ps);
    rtc_peer_connection_on_data_channel(pc, on_data_channel, ps);

    ps->pc = pc;
    return pc;
}

/* ------------------------------------------------------------------ */
/*  Create offer and send to peer via signaling                        */
/* ------------------------------------------------------------------ */
static int offer_to_peer(peer_state_t *ps) {
    rtc_peer_connection_t *pc = create_pc(ps);
    if (!pc)
        return -1;

    /* Create data channel (offerer side) */
    rtc_data_channel_init_t dc_opts;
    memset(&dc_opts, 0, sizeof(dc_opts));
    dc_opts.ordered = true;
    dc_opts.max_retransmits = -1;
    dc_opts.max_packet_life = -1;
    dc_opts.id = -1;
    ps->dc = rtc_peer_connection_create_data_channel(pc, "chat", &dc_opts);
    if (ps->dc)
        setup_dc_callbacks(ps);

    /* Create and set local offer */
    rtc_desc_t offer;
    int rc = rtc_peer_connection_create_offer(pc, &offer);
    if (rc != RTC_OK)
        return rc;

    rc = rtc_peer_connection_set_local_desc(pc, &offer);
    if (rc != RTC_OK)
        return rc;

    /* Send offer via signaling */
    signaling_send_offer(g_signaling, ps->peer_id, offer.sdp);
    printf("  [sent offer to %s]\n", ps->peer_id);
    fflush(stdout);
    return RTC_OK;
}

/* ------------------------------------------------------------------ */
/*  Handle incoming offer: create answer and send back                 */
/* ------------------------------------------------------------------ */
static int handle_offer(const char *from, const char *sdp) {
    peer_state_t *ps = find_peer(from);
    if (!ps) {
        ps = add_peer(from);
        if (!ps)
            return -1;
    }

    rtc_peer_connection_t *pc = create_pc(ps);
    if (!pc)
        return -1;

    /* Set remote offer */
    rtc_desc_t offer_desc;
    memset(&offer_desc, 0, sizeof(offer_desc));
    offer_desc.type = RTC_SDP_OFFER;
    size_t sdp_len = strlen(sdp);
    if (sdp_len >= SDP_MAX_SIZE)
        sdp_len = SDP_MAX_SIZE - 1;
    memcpy(offer_desc.sdp, sdp, sdp_len);
    offer_desc.sdp_len = sdp_len;

    int rc = rtc_peer_connection_set_remote_desc(pc, &offer_desc);
    if (rc != RTC_OK)
        return rc;

    /* Create and set local answer */
    rtc_desc_t answer;
    rc = rtc_peer_connection_create_answer(pc, &answer);
    if (rc != RTC_OK)
        return rc;

    rc = rtc_peer_connection_set_local_desc(pc, &answer);
    if (rc != RTC_OK)
        return rc;

    /* Send answer via signaling */
    signaling_send_answer(g_signaling, from, answer.sdp);
    printf("  [sent answer to %s]\n", from);
    fflush(stdout);
    return RTC_OK;
}

/* ------------------------------------------------------------------ */
/*  Handle incoming answer                                             */
/* ------------------------------------------------------------------ */
static int handle_answer(const char *from, const char *sdp) {
    peer_state_t *ps = find_peer(from);
    if (!ps || !ps->pc)
        return -1;

    rtc_desc_t answer_desc;
    memset(&answer_desc, 0, sizeof(answer_desc));
    answer_desc.type = RTC_SDP_ANSWER;
    size_t sdp_len = strlen(sdp);
    if (sdp_len >= SDP_MAX_SIZE)
        sdp_len = SDP_MAX_SIZE - 1;
    memcpy(answer_desc.sdp, sdp, sdp_len);
    answer_desc.sdp_len = sdp_len;

    return rtc_peer_connection_set_remote_desc(ps->pc, &answer_desc);
}

/* ------------------------------------------------------------------ */
/*  Signaling callbacks                                                */
/* ------------------------------------------------------------------ */
static void on_joined(const char *my_id, const char **peer_ids, int peer_count, void *user) {
    (void)user;
    snprintf(g_my_id, sizeof(g_my_id), "%s", my_id);
    printf("  [joined as %s, %d peer(s) already here]\n", my_id, peer_count);
    fflush(stdout);

    /* I'm the new joiner — send offers to all existing peers */
    for (int i = 0; i < peer_count; i++) {
        peer_state_t *ps = add_peer(peer_ids[i]);
        if (ps)
            offer_to_peer(ps);
    }
}

static void on_peer_joined(const char *peer_id, void *user) {
    (void)user;
    printf("  [%s joined — waiting for their offer]\n", peer_id);
    fflush(stdout);
    /* They will offer to us since they are newer */
}

static void on_peer_left(const char *peer_id, void *user) {
    (void)user;
    remove_peer(peer_id);
}

static void on_offer(const char *from, const char *sdp, void *user) {
    (void)user;
    handle_offer(from, sdp);
}

static void on_answer(const char *from, const char *sdp, void *user) {
    (void)user;
    handle_answer(from, sdp);
}

static void on_candidate(const char *from, const char *candidate, void *user) {
    (void)user;
    (void)from;
    (void)candidate;
    /* Trickle ICE not implemented — candidates are in the SDP */
}

static void on_error(const char *message, void *user) {
    (void)user;
    printf("  [error] %s\n", message);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/*  Send a chat message to all connected peers                         */
/* ------------------------------------------------------------------ */
static void send_to_all(const char *text, size_t len) {
    for (int i = 0; i < g_peer_count; i++) {
        if (g_peers[i].active && g_peers[i].dc_open && g_peers[i].dc) {
            rtc_data_channel_send(g_peers[i].dc, (const uint8_t *)text, len);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Non-blocking stdin check                                           */
/* ------------------------------------------------------------------ */
static bool stdin_has_data(void) {
#ifdef _WIN32
    return _kbhit() != 0;
#else
    fd_set fds;
    struct timeval tv = {0, 0};
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
#endif
}

/* ------------------------------------------------------------------ */
/*  Usage                                                              */
/* ------------------------------------------------------------------ */
static void print_usage(const char *prog) {
    printf("Usage: %s --meeting <name> [--server <url>] [--stun <ip>]\n\n", prog);
    printf("  --meeting <name>   Meeting to join (required)\n");
    printf("  --server <url>     Signaling server (default: ws://127.0.0.1:8080)\n");
    printf("  --stun <ip>        STUN server IP (default: stun.l.google.com)\n");
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[]) {
    const char *meeting = NULL;
    const char *server_url = "ws://127.0.0.1:8080";

    /* Parse args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--meeting") == 0 && i + 1 < argc)
            meeting = argv[++i];
        else if (strcmp(argv[i], "--server") == 0 && i + 1 < argc)
            server_url = argv[++i];
        else if (strcmp(argv[i], "--stun") == 0 && i + 1 < argc)
            g_stun_ip = argv[++i];
        else {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!meeting) {
        print_usage(argv[0]);
        return 1;
    }

    printf("================================================\n");
    printf("  mrtc - WebRTC Chat (Data Channels)\n");
    printf("================================================\n");
    printf("  Meeting:  %s\n", meeting);
    printf("  Server:   %s\n", server_url);
    printf("  STUN:     %s\n", g_stun_ip);
    printf("================================================\n\n");

    /* Initialize mrtc */
    rtc_init();

    /* Create signaling client */
    signaling_config_t sig_cfg;
    memset(&sig_cfg, 0, sizeof(sig_cfg));
    sig_cfg.server_url = server_url;
    sig_cfg.meeting = meeting;
    sig_cfg.on_joined = on_joined;
    sig_cfg.on_peer_joined = on_peer_joined;
    sig_cfg.on_peer_left = on_peer_left;
    sig_cfg.on_offer = on_offer;
    sig_cfg.on_answer = on_answer;
    sig_cfg.on_candidate = on_candidate;
    sig_cfg.on_error = on_error;

    g_signaling = signaling_create(&sig_cfg);
    if (!g_signaling) {
        printf("  [FAIL] Could not create signaling client\n");
        return 1;
    }

    /* Connect to signaling server (auto-joins meeting) */
    int rc = signaling_connect(g_signaling);
    if (rc != RTC_OK) {
        printf("  [FAIL] Could not connect to signaling server: %s\n", server_url);
        printf("  Make sure the signaling server is running:\n");
        printf("    ./signaling/signaling_server 8080\n");
        signaling_destroy(g_signaling);
        return 1;
    }

    /* Wait a moment for connection + join */
    SLEEP_MS(500);

    printf("  Chat ready! Type messages and press Enter.\n");
    printf("  Type 'quit' to leave.\n\n");
    fflush(stdout);

    /* Chat loop */
    char input[1024];
    while (g_running) {
        if (stdin_has_data()) {
#ifdef _WIN32
            /* Windows: line-buffered via fgets after _kbhit detects Enter */
            if (fgets(input, sizeof(input), stdin)) {
                size_t len = strlen(input);
                while (len > 0 && (input[len - 1] == '\n' || input[len - 1] == '\r'))
                    input[--len] = '\0';
                if (len > 0) {
                    if (strcmp(input, "quit") == 0)
                        break;
                    printf("  [me] %s\n", input);
                    fflush(stdout);
                    send_to_all(input, len);
                }
            }
#else
            if (fgets(input, sizeof(input), stdin)) {
                size_t len = strlen(input);
                while (len > 0 && (input[len - 1] == '\n' || input[len - 1] == '\r'))
                    input[--len] = '\0';
                if (len > 0) {
                    if (strcmp(input, "quit") == 0)
                        break;
                    printf("  [me] %s\n", input);
                    fflush(stdout);
                    send_to_all(input, len);
                }
            }
#endif
        }
        SLEEP_MS(10);
    }

    /* Cleanup */
    printf("\n  Leaving meeting...\n");
    signaling_leave(g_signaling);
    SLEEP_MS(200);

    for (int i = 0; i < g_peer_count; i++) {
        if (g_peers[i].active && g_peers[i].pc) {
            rtc_peer_connection_close(g_peers[i].pc);
            rtc_peer_connection_destroy(g_peers[i].pc);
        }
    }

    signaling_destroy(g_signaling);
    rtc_cleanup();

    printf("  Bye!\n");
    return 0;
}
