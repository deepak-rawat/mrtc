/*
 * DTLS component tests.
 *
 * Tests:
 *   1. Init DTLS transport (client), verify fingerprint generated
 *   2. Init DTLS transport (server), verify fingerprint generated
 *   3. Client/server fingerprints are different (unique certs)
 *   4. Full handshake between client and server via memory BIOs
 *   5. SRTP key export after handshake
 *   6. Client and server derive the same SRTP keys
 */
#include <rtc/rtc.h>
#include "rtc/rtc_rtp.h"
#include "rtc_dtls.h"
#include "rtc_srtp.h"
#include "test_harness.h"

/* Buffer to shuttle DTLS packets between client and server */
typedef struct {
    uint8_t data[4096];
    size_t len;
    bool has_data;
} dtls_pipe_t;

static dtls_pipe_t pipe_to_server;
static dtls_pipe_t pipe_to_client;

static int client_send_fn(const uint8_t *data, size_t len, void *user) {
    (void)user;
    if (len > sizeof(pipe_to_server.data))
        return RTC_ERR_GENERIC;
    memcpy(pipe_to_server.data, data, len);
    pipe_to_server.len = len;
    pipe_to_server.has_data = true;
    return RTC_OK;
}

static int server_send_fn(const uint8_t *data, size_t len, void *user) {
    (void)user;
    if (len > sizeof(pipe_to_client.data))
        return RTC_ERR_GENERIC;
    memcpy(pipe_to_client.data, data, len);
    pipe_to_client.len = len;
    pipe_to_client.has_data = true;
    return RTC_OK;
}

TEST(dtls_init_client) {
    rtc_dtls_transport_t dtls;
    int rc = rtc_dtls_init(&dtls, RTC_DTLS_ROLE_CLIENT, NULL, NULL);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT_EQ(dtls.role, RTC_DTLS_ROLE_CLIENT);
    ASSERT_EQ(dtls.state, RTC_DTLS_STATE_NEW);
    ASSERT(dtls.ctx != NULL);
    ASSERT(dtls.ssl != NULL);

    /* Fingerprint should be a non-empty SHA-256 hex string with colons */
    const char *fp = rtc_dtls_get_fingerprint(&dtls);
    ASSERT(fp != NULL);
    ASSERT(strlen(fp) > 0);
    /* SHA-256 = 32 bytes = 95 chars (XX:XX:...:XX) */
    ASSERT_EQ(strlen(fp), 95);
    ASSERT_EQ(fp[2], ':');

    printf("    client fingerprint: %s\n", fp);
    rtc_dtls_close(&dtls);
}

TEST(dtls_init_server) {
    rtc_dtls_transport_t dtls;
    int rc = rtc_dtls_init(&dtls, RTC_DTLS_ROLE_SERVER, NULL, NULL);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT_EQ(dtls.role, RTC_DTLS_ROLE_SERVER);

    const char *fp = rtc_dtls_get_fingerprint(&dtls);
    ASSERT(strlen(fp) == 95);

    printf("    server fingerprint: %s\n", fp);
    rtc_dtls_close(&dtls);
}

TEST(dtls_unique_certs) {
    rtc_dtls_transport_t a, b;
    rtc_dtls_init(&a, RTC_DTLS_ROLE_CLIENT, NULL, NULL);
    rtc_dtls_init(&b, RTC_DTLS_ROLE_CLIENT, NULL, NULL);

    ASSERT(strcmp(rtc_dtls_get_fingerprint(&a), rtc_dtls_get_fingerprint(&b)) != 0);

    printf("    two clients have different fingerprints\n");
    rtc_dtls_close(&a);
    rtc_dtls_close(&b);
}

TEST(dtls_role_change_keeps_fingerprint) {
    rtc_dtls_transport_t dtls;
    int rc = rtc_dtls_init(&dtls, RTC_DTLS_ROLE_CLIENT, client_send_fn, NULL);
    ASSERT_EQ(rc, RTC_OK);

    char fp_before[RTC_DTLS_FINGERPRINT_SIZE];
    memcpy(fp_before, rtc_dtls_get_fingerprint(&dtls), sizeof(fp_before));

    rc = rtc_dtls_set_role(&dtls, RTC_DTLS_ROLE_SERVER);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT_EQ(dtls.role, RTC_DTLS_ROLE_SERVER);
    ASSERT_EQ(dtls.state, RTC_DTLS_STATE_NEW);
    ASSERT_STR_EQ(fp_before, rtc_dtls_get_fingerprint(&dtls));

    printf("    role switch kept fingerprint: %.20s...\n", rtc_dtls_get_fingerprint(&dtls));
    rtc_dtls_close(&dtls);
}

TEST(dtls_handshake) {
    memset(&pipe_to_server, 0, sizeof(pipe_to_server));
    memset(&pipe_to_client, 0, sizeof(pipe_to_client));

    rtc_dtls_transport_t client, server;
    int rc;

    rc = rtc_dtls_init(&client, RTC_DTLS_ROLE_CLIENT, client_send_fn, NULL);
    ASSERT_EQ(rc, RTC_OK);
    rc = rtc_dtls_init(&server, RTC_DTLS_ROLE_SERVER, server_send_fn, NULL);
    ASSERT_EQ(rc, RTC_OK);

    printf("    client fp: %.20s...\n", rtc_dtls_get_fingerprint(&client));
    printf("    server fp: %.20s...\n", rtc_dtls_get_fingerprint(&server));

    /* Kick off the handshake from the client side */
    rc = rtc_dtls_handshake(&client);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT(pipe_to_server.has_data); /* ClientHello should be queued */

    /* Drive the handshake: shuttle data between client and server */
    int max_rounds = 20;
    for (int round = 0; round < max_rounds; round++) {
        bool progress = false;

        if (pipe_to_server.has_data) {
            pipe_to_server.has_data = false;
            rc = rtc_dtls_recv(&server, pipe_to_server.data, pipe_to_server.len);
            ASSERT_EQ(rc, RTC_OK);
            progress = true;
        }

        if (pipe_to_client.has_data) {
            pipe_to_client.has_data = false;
            rc = rtc_dtls_recv(&client, pipe_to_client.data, pipe_to_client.len);
            ASSERT_EQ(rc, RTC_OK);
            progress = true;
        }

        if (client.state == RTC_DTLS_STATE_CONNECTED && server.state == RTC_DTLS_STATE_CONNECTED) {
            printf("    handshake complete in %d rounds\n", round + 1);
            break;
        }

        ASSERT(progress || round < max_rounds - 1);
    }

    ASSERT_EQ(client.state, RTC_DTLS_STATE_CONNECTED);
    ASSERT_EQ(server.state, RTC_DTLS_STATE_CONNECTED);

    rtc_dtls_close(&client);
    rtc_dtls_close(&server);
}

TEST(dtls_srtp_key_export) {
    memset(&pipe_to_server, 0, sizeof(pipe_to_server));
    memset(&pipe_to_client, 0, sizeof(pipe_to_client));

    rtc_dtls_transport_t client, server;
    rtc_dtls_init(&client, RTC_DTLS_ROLE_CLIENT, client_send_fn, NULL);
    rtc_dtls_init(&server, RTC_DTLS_ROLE_SERVER, server_send_fn, NULL);

    /* Complete handshake */
    rtc_dtls_handshake(&client);
    for (int i = 0; i < 20; i++) {
        if (pipe_to_server.has_data) {
            pipe_to_server.has_data = false;
            rtc_dtls_recv(&server, pipe_to_server.data, pipe_to_server.len);
        }
        if (pipe_to_client.has_data) {
            pipe_to_client.has_data = false;
            rtc_dtls_recv(&client, pipe_to_client.data, pipe_to_client.len);
        }
        if (client.state == RTC_DTLS_STATE_CONNECTED && server.state == RTC_DTLS_STATE_CONNECTED)
            break;
    }
    ASSERT_EQ(client.state, RTC_DTLS_STATE_CONNECTED);
    ASSERT_EQ(server.state, RTC_DTLS_STATE_CONNECTED);

    /* Export SRTP keys */
    int rc = rtc_dtls_export_srtp_keys(&client);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT(client.srtp_keys_ready);

    rc = rtc_dtls_export_srtp_keys(&server);
    ASSERT_EQ(rc, RTC_OK);
    ASSERT(server.srtp_keys_ready);

    /* Both sides should derive the same keying material */
    ASSERT_MEM_EQ(client.srtp_client_key, server.srtp_client_key, RTC_SRTP_MASTER_KEY_LEN);
    ASSERT_MEM_EQ(client.srtp_server_key, server.srtp_server_key, RTC_SRTP_MASTER_KEY_LEN);
    ASSERT_MEM_EQ(client.srtp_client_salt, server.srtp_client_salt, RTC_SRTP_MASTER_SALT_LEN);
    ASSERT_MEM_EQ(client.srtp_server_salt, server.srtp_server_salt, RTC_SRTP_MASTER_SALT_LEN);

    /* Client key != server key (they're independently derived) */
    ASSERT(memcmp(client.srtp_client_key, client.srtp_server_key, RTC_SRTP_MASTER_KEY_LEN) != 0);

    printf("    client_key[0..3]: %02X %02X %02X %02X\n", client.srtp_client_key[0],
           client.srtp_client_key[1], client.srtp_client_key[2], client.srtp_client_key[3]);
    printf("    server_key[0..3]: %02X %02X %02X %02X\n", client.srtp_server_key[0],
           client.srtp_server_key[1], client.srtp_server_key[2], client.srtp_server_key[3]);
    printf("    both sides derived identical keys\n");

    rtc_dtls_close(&client);
    rtc_dtls_close(&server);
}

TEST(dtls_srtp_e2e) {
    memset(&pipe_to_server, 0, sizeof(pipe_to_server));
    memset(&pipe_to_client, 0, sizeof(pipe_to_client));

    rtc_dtls_transport_t client, server;
    rtc_dtls_init(&client, RTC_DTLS_ROLE_CLIENT, client_send_fn, NULL);
    rtc_dtls_init(&server, RTC_DTLS_ROLE_SERVER, server_send_fn, NULL);

    /* Handshake */
    rtc_dtls_handshake(&client);
    for (int i = 0; i < 20; i++) {
        if (pipe_to_server.has_data) {
            pipe_to_server.has_data = false;
            rtc_dtls_recv(&server, pipe_to_server.data, pipe_to_server.len);
        }
        if (pipe_to_client.has_data) {
            pipe_to_client.has_data = false;
            rtc_dtls_recv(&client, pipe_to_client.data, pipe_to_client.len);
        }
        if (client.state == RTC_DTLS_STATE_CONNECTED && server.state == RTC_DTLS_STATE_CONNECTED)
            break;
    }
    ASSERT_EQ(client.state, RTC_DTLS_STATE_CONNECTED);

    /* Export keys on both sides */
    rtc_dtls_export_srtp_keys(&client);
    rtc_dtls_export_srtp_keys(&server);

    /* Client sends to server: client encrypts with client_key, server decrypts with client_key */
    rtc_srtp_ctx_t client_send, server_recv;
    rtc_srtp_init(&client_send, client.srtp_client_key, RTC_SRTP_MASTER_KEY_LEN,
                  client.srtp_client_salt, RTC_SRTP_MASTER_SALT_LEN);
    rtc_srtp_init(&server_recv, server.srtp_client_key, RTC_SRTP_MASTER_KEY_LEN,
                  server.srtp_client_salt, RTC_SRTP_MASTER_SALT_LEN);

    /* Build, protect, unprotect */
    const char *message = "Hello from DTLS+SRTP!";
    rtc_rtp_packet_t pkt;
    rtc_rtp_build(&pkt, 111, 1, 0, 0xABCD1234, false, (const uint8_t *)message, strlen(message));

    uint8_t buf[1500];
    memcpy(buf, pkt.buf, pkt.buf_len);
    size_t len = pkt.buf_len;

    int rc = rtc_srtp_protect(&client_send, buf, &len, sizeof(buf));
    ASSERT_EQ(rc, RTC_OK);

    rc = rtc_srtp_unprotect(&server_recv, buf, &len);
    ASSERT_EQ(rc, RTC_OK);

    ASSERT_EQ(len, pkt.buf_len);
    ASSERT_MEM_EQ(buf + RTP_HEADER_SIZE, message, strlen(message));

    printf("    DTLS handshake + SRTP encrypt/decrypt: \"%s\"\n", message);

    rtc_srtp_close(&client_send);
    rtc_srtp_close(&server_recv);
    rtc_dtls_close(&client);
    rtc_dtls_close(&server);
}

int main(void) {
    printf("========================================\n");
    printf("  DTLS Component Tests\n");
    printf("========================================\n\n");

    rtc_init();
    rtc_set_log_level(RTC_LOG_DEBUG);

    RUN_TEST(dtls_init_client);
    RUN_TEST(dtls_init_server);
    RUN_TEST(dtls_unique_certs);
    RUN_TEST(dtls_role_change_keeps_fingerprint);
    RUN_TEST(dtls_handshake);
    RUN_TEST(dtls_srtp_key_export);
    RUN_TEST(dtls_srtp_e2e);

    rtc_cleanup();
    TEST_SUMMARY();
}
