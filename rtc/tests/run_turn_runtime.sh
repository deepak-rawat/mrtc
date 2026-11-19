#!/bin/sh
set -eu
umask 077

server=$1
test_binary=$2
mode=${3:-udp}
if [ ! -x "$server" ]; then
    echo "SKIP: coturn executable unavailable"
    exit 77
fi

port=$((20000 + $$ % 10000))
relay_min=$((40000 + ($$ % 1000) * 20))
relay_max=$((relay_min + 19))
log="coturn-runtime-$$.log"
pidfile="coturn-runtime-$$.pid"
server_pid=
test_pid=
request="coturn-runtime-$$.restart"
ready="coturn-runtime-$$.ready"
certificate="coturn-runtime-$$.pem"
private_key="coturn-runtime-$$.key"
dtls_flag=
if "$server" --help 2>&1 | grep -q -- '--no-dtls'; then
    dtls_flag=--no-dtls
fi
stop_server() {
    if [ -n "$server_pid" ]; then
        kill "$server_pid" 2>/dev/null || true
        tries=0
        while kill -0 "$server_pid" 2>/dev/null && [ "$tries" -lt 20 ]; do
            sleep 0.1
            tries=$((tries + 1))
        done
        if kill -0 "$server_pid" 2>/dev/null; then
            kill -KILL "$server_pid" 2>/dev/null || true
        fi
        wait "$server_pid" 2>/dev/null || true
        server_pid=
    fi
}
cleanup() {
    if [ -n "$test_pid" ]; then
        kill "$test_pid" 2>/dev/null || true
        wait "$test_pid" 2>/dev/null || true
    fi
    stop_server
    rm -f "$log" "$pidfile" "$certificate" "$private_key" "$request" "$ready"
}
trap cleanup EXIT HUP INT TERM

set -- --no-tcp --no-tls
export MRTC_TEST_TURN_TRANSPORT=udp
unset MRTC_TEST_TURN_CA MRTC_TEST_TURN_EXPECT_REJECT MRTC_TEST_TURN_FAILOVER MRTC_TEST_TURN_EXHAUST MRTC_TEST_TURN_STALL MRTC_TEST_TURN_DNS MRTC_TEST_TURN_FOUR MRTC_TEST_TURN_IDENTITIES MRTC_TEST_TURN_RESTART MRTC_TEST_TURN_READY MRTC_TEST_TURN_REMOTE_RESTART MRTC_TEST_TURN_FAMILY_FAIR
export MRTC_TEST_TURN_HOST=127.0.0.1
case "$mode" in
    *-ipv6) export MRTC_TEST_TURN_HOST=::1 ;;
    *-identities) export MRTC_TEST_TURN_IDENTITIES=1 ;;
    *-restart) export MRTC_TEST_TURN_RESTART="$request" MRTC_TEST_TURN_READY="$ready" ;;
esac
export MRTC_TEST_TURN_RELAY="$MRTC_TEST_TURN_HOST"
case "$mode" in
    udp-remote-restart) export MRTC_TEST_TURN_REMOTE_RESTART=1 ;;
    udp-family-identities) export MRTC_TEST_TURN_FAMILY_FAIR=1 ;;
esac
case "$mode" in
    udp-v4-v6) export MRTC_TEST_TURN_RELAY=::1 ;;
    udp-v6-v4) export MRTC_TEST_TURN_HOST=::1 MRTC_TEST_TURN_RELAY=127.0.0.1 ;;
esac
case "$mode" in
    *-failover*) export MRTC_TEST_TURN_FAILOVER=1 ;;
    udp-exhausted) export MRTC_TEST_TURN_FAILOVER=1 MRTC_TEST_TURN_EXHAUST=1 ;;
esac
case "$mode" in
    tcp-stall-failover|tls-stall-failover) export MRTC_TEST_TURN_STALL=1 ;;
    tls-dns) export MRTC_TEST_TURN_DNS=1 ;;
    udp-four-failover) export MRTC_TEST_TURN_FOUR=1 ;;
esac
client_port=$port
case "$mode" in
    udp|udp-failover|udp-four-failover|udp-exhausted|udp-ipv6|udp-identities|udp-family-identities|udp-restart|udp-remote-restart|udp-v4-v6|udp-v6-v4) ;;
    tcp|tcp-failover|tcp-stall-failover|tcp-ipv6|tcp-restart)
        set -- --no-udp --no-tls
        export MRTC_TEST_TURN_TRANSPORT=tcp
        ;;
    tls|tls-dns|tls-untrusted|tls-wrong-name|tls-failover|tls-stall-failover|tls-failover-untrusted|tls-failover-wrong-name|tls-ipv6|tls-identities|tls-restart)
        san="IP:127.0.0.1,IP:::1,DNS:localhost"
        case "$mode" in tls-identities) san="DNS:localhost" ;; esac
        case "$mode" in *wrong-name) san="DNS:wrong.example" ;; esac
        openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 1 \
            -subj /CN=localhost -addext "subjectAltName=$san" \
            -keyout "$private_key" -out "$certificate" > "$log" 2>&1
        client_port=$((port + 1))
        set -- --no-udp --tls-listening-port="$client_port" \
            --cert="$certificate" --pkey="$private_key"
        export MRTC_TEST_TURN_TRANSPORT=tls
        case "$mode" in *untrusted) ;; *) export MRTC_TEST_TURN_CA="$certificate" ;; esac
        case "$mode" in tls|tls-dns|tls-failover|tls-stall-failover|tls-ipv6|tls-identities|tls-restart) ;; *) export MRTC_TEST_TURN_EXPECT_REJECT=1 ;; esac
        ;;
    *) echo "Unsupported TURN fixture mode: $mode"; exit 1 ;;
esac

case "$mode" in
    udp-identities|tls-identities) set -- "$@" --listening-ip=::1 ;;
esac

start_server() {
"$server" -n --listening-ip="$MRTC_TEST_TURN_HOST" --relay-ip="$MRTC_TEST_TURN_RELAY" \
    --listening-port="$port" --min-port="$relay_min" --max-port="$relay_max" \
    --realm=mrtc.test --lt-cred-mech --user=mrtc:runtime \
    "$@" --no-tcp-relay $dtls_flag --no-cli \
    --allow-loopback-peers --no-multicast-peers --relay-threads=1 \
    --max-allocate-lifetime=2 --channel-lifetime=3 --permission-lifetime=2 \
    --pidfile="$pidfile" --log-file=stdout > "$log" 2>&1 &
server_pid=$!
sleep 1
if ! kill -0 "$server_pid" 2>/dev/null; then
    cat "$log"
    exit 1
fi
}
start_server "$@"
MRTC_TEST_TURN_PORT="$client_port" "$test_binary" --turn &
test_pid=$!
while kill -0 "$test_pid" 2>/dev/null; do
    if [ -f "$request" ]; then
        stop_server
        start_server "$@"
        rm -f "$request"
        : > "$ready"
    fi
    sleep 0.05
done
status=0
wait "$test_pid" || status=$?
test_pid=
if [ "$status" -eq 0 ]; then
    exit 0
fi
cat "$log"
exit 1
