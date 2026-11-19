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
certificate="coturn-runtime-$$.pem"
private_key="coturn-runtime-$$.key"
dtls_flag=
if "$server" --help 2>&1 | grep -q -- '--no-dtls'; then
    dtls_flag=--no-dtls
fi
cleanup() {
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
    rm -f "$log" "$pidfile" "$certificate" "$private_key"
}
trap cleanup EXIT HUP INT TERM

set -- --no-tcp --no-tls
export MRTC_TEST_TURN_TRANSPORT=udp
unset MRTC_TEST_TURN_CA MRTC_TEST_TURN_EXPECT_REJECT MRTC_TEST_TURN_FAILOVER MRTC_TEST_TURN_EXHAUST MRTC_TEST_TURN_STALL MRTC_TEST_TURN_DNS MRTC_TEST_TURN_FOUR
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
    udp|udp-failover|udp-four-failover|udp-exhausted) ;;
    tcp|tcp-failover|tcp-stall-failover)
        set -- --no-udp --no-tls
        export MRTC_TEST_TURN_TRANSPORT=tcp
        ;;
    tls|tls-dns|tls-untrusted|tls-wrong-name|tls-failover|tls-stall-failover|tls-failover-untrusted|tls-failover-wrong-name)
        san="IP:127.0.0.1,DNS:localhost"
        case "$mode" in *wrong-name) san="DNS:wrong.example" ;; esac
        openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 1 \
            -subj /CN=localhost -addext "subjectAltName=$san" \
            -keyout "$private_key" -out "$certificate" > "$log" 2>&1
        client_port=$((port + 1))
        set -- --no-udp --tls-listening-port="$client_port" \
            --cert="$certificate" --pkey="$private_key"
        export MRTC_TEST_TURN_TRANSPORT=tls
        case "$mode" in *untrusted) ;; *) export MRTC_TEST_TURN_CA="$certificate" ;; esac
        case "$mode" in tls|tls-dns|tls-failover|tls-stall-failover) ;; *) export MRTC_TEST_TURN_EXPECT_REJECT=1 ;; esac
        ;;
    *) echo "Unsupported TURN fixture mode: $mode"; exit 1 ;;
esac

"$server" -n --listening-ip=127.0.0.1 --relay-ip=127.0.0.1 \
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
if MRTC_TEST_TURN_PORT="$client_port" "$test_binary" --turn; then
    exit 0
fi
cat "$log"
exit 1
