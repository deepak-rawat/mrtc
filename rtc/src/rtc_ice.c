/*
 * ICE (RFC 8445) minimal implementation.
 *
 * Gathers host candidates from local interfaces and server-reflexive
 * candidates via STUN. Performs simplified connectivity checks.
 * Uses an rtc_packet_io_t for all socket I/O.
 */
#include "rtc_ice.h"
#include "rtc_packet_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <iphlpapi.h>
#else
#  include <ifaddrs.h>
#  include <net/if.h>
#endif

/* Big-endian write helpers */
static void write_u16_be(uint8_t *p, uint16_t v) {
    p[0] = (v >> 8) & 0xFF;
    p[1] = v & 0xFF;
}
static void write_u32_be(uint8_t *p, uint32_t v) {
    p[0] = (v >> 24) & 0xFF;
    p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8) & 0xFF;
    p[3] = v & 0xFF;
}

/* Compute ICE priority per RFC 8445 section 5.1.2.1 */
static uint32_t ice_candidate_priority(rtc_ice_candidate_type_t type, int local_pref,
                                       int component) {
    uint32_t type_pref;
    switch (type) {
        case ICE_CANDIDATE_HOST:
            type_pref = 126;
            break;
        case ICE_CANDIDATE_SRFLX:
            type_pref = 100;
            break;
        default:
            type_pref = 0;
            break;
    }
    return (type_pref << 24) | ((uint32_t)local_pref << 8) | (256 - (uint32_t)component);
}

int rtc_ice_init(rtc_ice_agent_t *agent, rtc_packet_io_t *transport, const char *stun_server,
                 uint16_t stun_port) {
    memset(agent, 0, sizeof(*agent));
    agent->state = ICE_STATE_NEW;
    agent->transport = transport;

    /* Generate credentials */
    rtc_random_string(agent->ufrag, ICE_UFRAG_LEN);
    rtc_random_string(agent->pwd, ICE_PWD_LEN);

    /* STUN config */
    if (stun_server) {
        size_t slen = strlen(stun_server);
        if (slen >= sizeof(agent->stun_server))
            slen = sizeof(agent->stun_server) - 1;
        memcpy(agent->stun_server, stun_server, slen);
        agent->stun_server[slen] = '\0';
    }
    agent->stun_port = stun_port ? stun_port : 3478;

    /* ICE-controlling by default */
    agent->controlling = true;
    rtc_random_bytes((uint8_t *)&agent->tie_breaker, sizeof(agent->tie_breaker));

    RTC_LOG_INFO("ICE agent initialized: ufrag=%s", agent->ufrag);
    return RTC_OK;
}

int rtc_ice_restart_credentials(rtc_ice_agent_t *agent) {
    if (!agent)
        return RTC_ERR_INVALID;
    rtc_random_string(agent->ufrag, ICE_UFRAG_LEN);
    rtc_random_string(agent->pwd, ICE_PWD_LEN);
    RTC_LOG_INFO("ICE credentials restarted: ufrag=%s", agent->ufrag);
    return RTC_OK;
}

/* Gather host candidates from local interfaces */
static int ice_gather_host(rtc_ice_agent_t *agent) {
    /* Get the port we bound to from the transport socket. Use
     * sockaddr_storage because the transport's socket is AF_INET6
     * (dual-stack) and a sockaddr_in is too small. sin_port / sin6_port
     * happen to live at the same byte offset, so we can read the port
     * via a sockaddr_in cast on the properly-sized buffer. */
    rtc_socket_t sock = rtc_packet_io_get_socket(agent->transport);
    struct sockaddr_storage local_ss;
    socklen_t local_len = sizeof(local_ss);
    if (getsockname(sock, (struct sockaddr *)&local_ss, &local_len) != 0)
        return RTC_ERR_SOCKET;
    uint16_t port = ntohs(((struct sockaddr_in *)&local_ss)->sin_port);

#ifdef _WIN32
    /* Windows: use GetAdaptersAddresses */
    ULONG bufsize = 15000;
    PIP_ADAPTER_ADDRESSES addrs = (PIP_ADAPTER_ADDRESSES)malloc(bufsize);
    if (!addrs)
        return RTC_ERR_NOMEM;

    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG ret = GetAdaptersAddresses(AF_INET, flags, NULL, addrs, &bufsize);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        free(addrs);
        addrs = (PIP_ADAPTER_ADDRESSES)malloc(bufsize);
        if (!addrs)
            return RTC_ERR_NOMEM;
        ret = GetAdaptersAddresses(AF_INET, flags, NULL, addrs, &bufsize);
    }
    if (ret != NO_ERROR) {
        free(addrs);
        return RTC_ERR_GENERIC;
    }

    for (PIP_ADAPTER_ADDRESSES a = addrs; a && agent->local_candidate_count < ICE_MAX_CANDIDATES;
         a = a->Next) {
        if (a->OperStatus != IfOperStatusUp)
            continue;
        for (PIP_ADAPTER_UNICAST_ADDRESS ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
            struct sockaddr_in *sin = (struct sockaddr_in *)ua->Address.lpSockaddr;
            if (sin->sin_family != AF_INET)
                continue;
            if (sin->sin_addr.s_addr == htonl(INADDR_LOOPBACK))
                continue;

            rtc_ice_candidate_t *c = &agent->local_candidates[agent->local_candidate_count];
            c->type = ICE_CANDIDATE_HOST;
            c->component = 1;
            memset(&c->addr, 0, sizeof(c->addr));
            struct sockaddr_in *dst = (struct sockaddr_in *)&c->addr.addr;
            dst->sin_family = AF_INET;
            dst->sin_addr = sin->sin_addr;
            dst->sin_port = htons(port);
            c->addr.len = sizeof(struct sockaddr_in);
            c->priority = ice_candidate_priority(ICE_CANDIDATE_HOST, 65535, 1);
            snprintf(c->foundation, sizeof(c->foundation), "H%d", agent->local_candidate_count);
            agent->local_candidate_count++;

            char ip[64];
            uint16_t p;
            rtc_addr_to_string(&c->addr, ip, sizeof(ip), &p);
            RTC_LOG_INFO("ICE: host candidate %s:%u", ip, p);
        }
    }
    free(addrs);
#else
    /* Linux/macOS: use getifaddrs */
    struct ifaddrs *ifap, *ifa;
    if (getifaddrs(&ifap) != 0)
        return RTC_ERR_SOCKET;

    for (ifa = ifap; ifa && agent->local_candidate_count < ICE_MAX_CANDIDATES;
         ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr)
            continue;
        if (ifa->ifa_addr->sa_family != AF_INET)
            continue;
        if (ifa->ifa_flags & IFF_LOOPBACK)
            continue;
        if (!(ifa->ifa_flags & IFF_UP))
            continue;

        struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;

        rtc_ice_candidate_t *c = &agent->local_candidates[agent->local_candidate_count];
        c->type = ICE_CANDIDATE_HOST;
        c->component = 1;
        memset(&c->addr, 0, sizeof(c->addr));
        struct sockaddr_in *dst = (struct sockaddr_in *)&c->addr.addr;
        dst->sin_family = AF_INET;
        dst->sin_addr = sin->sin_addr;
        dst->sin_port = htons(port);
        c->addr.len = sizeof(struct sockaddr_in);
        c->priority = ice_candidate_priority(ICE_CANDIDATE_HOST, 65535, 1);
        snprintf(c->foundation, sizeof(c->foundation), "H%d", agent->local_candidate_count);
        agent->local_candidate_count++;

        char ip[64];
        uint16_t p;
        rtc_addr_to_string(&c->addr, ip, sizeof(ip), &p);
        RTC_LOG_INFO("ICE: host candidate %s:%u", ip, p);
    }
    freeifaddrs(ifap);
#endif

    return RTC_OK;
}

/* Gather server-reflexive candidate via STUN */
static int ice_gather_srflx(rtc_ice_agent_t *agent) {
    if (agent->stun_server[0] == '\0')
        return RTC_OK; /* no STUN server configured */

    rtc_addr_t mapped;
    rtc_socket_t sock = rtc_packet_io_get_socket(agent->transport);
    int rc = rtc_stun_binding(agent->stun_server, agent->stun_port, sock, &mapped);
    if (rc != RTC_OK) {
        RTC_LOG_WARN("ICE: STUN binding failed (rc=%d), continuing without srflx", rc);
        return RTC_OK; /* non-fatal */
    }

    if (agent->local_candidate_count >= ICE_MAX_CANDIDATES)
        return RTC_OK;

    rtc_ice_candidate_t *c = &agent->local_candidates[agent->local_candidate_count];
    c->type = ICE_CANDIDATE_SRFLX;
    c->component = 1;
    c->addr = mapped;
    c->priority = ice_candidate_priority(ICE_CANDIDATE_SRFLX, 65535, 1);
    snprintf(c->foundation, sizeof(c->foundation), "S%d", agent->local_candidate_count);
    agent->local_candidate_count++;

    char ip[64];
    uint16_t port;
    rtc_addr_to_string(&mapped, ip, sizeof(ip), &port);
    RTC_LOG_INFO("ICE: srflx candidate %s:%u", ip, port);

    return RTC_OK;
}

int rtc_ice_gather(rtc_ice_agent_t *agent) {
    agent->state = ICE_STATE_GATHERING;

    int rc = ice_gather_host(agent);
    if (rc != RTC_OK)
        return rc;

    rc = ice_gather_srflx(agent);
    if (rc != RTC_OK)
        return rc;

    if (agent->local_candidate_count == 0) {
        RTC_LOG_ERR("ICE: no candidates gathered");
        agent->state = ICE_STATE_FAILED;
        return RTC_ERR_ICE;
    }

    RTC_LOG_INFO("ICE: gathered %d candidate(s)", agent->local_candidate_count);
    return RTC_OK;
}

int rtc_ice_set_remote_credentials(rtc_ice_agent_t *agent, const char *ufrag, const char *pwd) {
    if (!ufrag || !pwd)
        return RTC_ERR_INVALID;
    size_t ulen = strlen(ufrag);
    size_t plen = strlen(pwd);
    if (ulen >= ICE_UFRAG_LEN || plen >= ICE_PWD_LEN)
        return RTC_ERR_INVALID;
    memcpy(agent->remote_ufrag, ufrag, ulen + 1);
    memcpy(agent->remote_pwd, pwd, plen + 1);
    return RTC_OK;
}

int rtc_ice_add_remote_candidate(rtc_ice_agent_t *agent, const rtc_ice_candidate_t *cand) {
    if (agent->remote_candidate_count >= ICE_MAX_CANDIDATES)
        return RTC_ERR_NOMEM;
    agent->remote_candidates[agent->remote_candidate_count++] = *cand;
    return RTC_OK;
}

/* Send a STUN Binding Success Response to a remote peer */
static void ice_send_binding_response(rtc_packet_io_t *transport, const uint8_t *req_buf,
                                      const rtc_addr_t *from) {
    uint8_t resp[128];
    size_t pos = STUN_HEADER_SIZE;

    /* XOR-MAPPED-ADDRESS attribute */
    const struct sockaddr_in *sin = (const struct sockaddr_in *)&from->addr;
    uint16_t xport = ntohs(sin->sin_port) ^ (uint16_t)(STUN_MAGIC_COOKIE >> 16);
    uint32_t xip = ntohl(sin->sin_addr.s_addr) ^ STUN_MAGIC_COOKIE;

    resp[pos++] = 0x00;
    resp[pos++] = 0x20; /* XOR-MAPPED-ADDRESS type */
    resp[pos++] = 0x00;
    resp[pos++] = 0x08; /* length */
    resp[pos++] = 0x00;
    resp[pos++] = 0x01; /* IPv4 */
    resp[pos++] = (xport >> 8) & 0xFF;
    resp[pos++] = xport & 0xFF;
    resp[pos++] = (xip >> 24) & 0xFF;
    resp[pos++] = (xip >> 16) & 0xFF;
    resp[pos++] = (xip >> 8) & 0xFF;
    resp[pos++] = xip & 0xFF;

    /* Header */
    write_u16_be(resp + 0, STUN_BINDING_RESPONSE);
    write_u16_be(resp + 2, (uint16_t)(pos - STUN_HEADER_SIZE));
    write_u32_be(resp + 4, STUN_MAGIC_COOKIE);
    memcpy(resp + 8, req_buf + 8, STUN_TXN_ID_SIZE); /* copy transaction ID */

    rtc_packet_io_send(transport, resp, pos, from);
}

int rtc_ice_send_check(rtc_ice_agent_t *agent) {
    if (agent->state != ICE_STATE_CHECKING)
        return RTC_ERR_ICE;
    if (agent->remote_candidate_count == 0)
        return RTC_ERR_ICE;

    int r = agent->check_round % agent->remote_candidate_count;
    rtc_ice_candidate_t *remote = &agent->remote_candidates[r];

    char ip[64];
    uint16_t port;
    rtc_addr_to_string(&remote->addr, ip, sizeof(ip), &port);
    if (agent->check_round < agent->remote_candidate_count)
        RTC_LOG_INFO("ICE: checking pair -> %s:%u", ip, port);
    else
        RTC_LOG_DBG("ICE: retry check -> %s:%u (round %d)", ip, port, agent->check_round);

    /* Build STUN Binding Request with ICE attributes */
    char username[ICE_UFRAG_LEN * 2 + 2];
    snprintf(username, sizeof(username), "%s:%s", agent->remote_ufrag, agent->ufrag);

    rtc_stun_msg_t req;
    uint32_t prio = ice_candidate_priority(ICE_CANDIDATE_HOST, 65535, 1);
    int rc =
        rtc_stun_build_binding_request(&req, username, agent->remote_pwd, prio, agent->controlling,
                                       agent->tie_breaker, agent->controlling);
    if (rc != RTC_OK) {
        agent->check_round++;
        return rc;
    }

    /* Remember the txn id so rtc_ice_handle_stun can match the response */
    memcpy(agent->last_txn_id, req.txn_id, STUN_TXN_ID_SIZE);

    rc = rtc_packet_io_send(agent->transport, req.buf, req.buf_len, &remote->addr);
    agent->check_round++;
    return rc;
}

bool rtc_ice_check_deadline_passed(const rtc_ice_agent_t *agent) {
    return rtc_time_ms() >= agent->check_deadline_ms;
}

int rtc_ice_connect(rtc_ice_agent_t *agent) {
    if (agent->remote_candidate_count == 0) {
        RTC_LOG_ERR("ICE: no remote candidates to check");
        agent->state = ICE_STATE_FAILED;
        return RTC_ERR_ICE;
    }

    agent->state = ICE_STATE_CHECKING;
    agent->check_round = 0;
    agent->check_deadline_ms = rtc_time_ms() + 30000; /* 30s overall */
    RTC_LOG_INFO("ICE: starting connectivity checks (%d remote candidates)",
                 agent->remote_candidate_count);

    /* Fire the first check synchronously; subsequent retries are scheduled by
     * the caller (e.g. via rtc_worker_add_timer) + rtc_ice_send_check. The
     * transport's recv callback dispatches BINDING_RESPONSE to
     * rtc_ice_handle_stun, which transitions state to CONNECTED. */
    return rtc_ice_send_check(agent);
}

/*
 * Handle an incoming STUN packet dispatched by the transport layer.
 * Processes binding requests (sends response) and binding responses.
 */
void rtc_ice_handle_stun(rtc_ice_agent_t *agent, const uint8_t *data, size_t len,
                         const rtc_addr_t *from) {
    if (len < STUN_HEADER_SIZE)
        return;

    uint32_t cookie =
        ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) | ((uint32_t)data[6] << 8) | data[7];
    if (cookie != STUN_MAGIC_COOKIE)
        return;

    uint16_t type = ((uint16_t)data[0] << 8) | data[1];

    if (type == STUN_BINDING_REQUEST) {
        ice_send_binding_response(agent->transport, data, from);
        RTC_LOG_DBG("ICE: handled binding request from peer");
        return;
    }

    if (type == STUN_BINDING_RESPONSE) {
        /* Only accept responses while we're actively checking. */
        if (agent->state != ICE_STATE_CHECKING)
            return;
        /* Match the transaction ID against our last outgoing request. */
        if (memcmp(data + 8, agent->last_txn_id, STUN_TXN_ID_SIZE) != 0) {
            RTC_LOG_DBG("ICE: binding response txn_id mismatch — ignoring");
            return;
        }

        /* Success — promote the responding remote to selected. */
        agent->selected_remote = *from;
        if (agent->local_candidate_count > 0)
            agent->selected_local = agent->local_candidates[0].addr;

        agent->state = ICE_STATE_CONNECTED;

        char ip[64];
        uint16_t port;
        rtc_addr_to_string(from, ip, sizeof(ip), &port);
        RTC_LOG_INFO("ICE: connected to %s:%u", ip, port);

        rtc_packet_io_set_remote(agent->transport, &agent->selected_remote);
    }
}

void rtc_ice_close(rtc_ice_agent_t *agent) {
    agent->state = ICE_STATE_CLOSED;
    agent->transport = NULL;
}
