#!/bin/sh
# Packet loss and delay between this host and one board, with Linux netem.
# Needs root (tc, ip). Only packets to or from BOARD_IP are affected.
#
#   sudo test/netsim/netem.sh on BOARD_IP loss 2% delay 30ms 10ms
#   sudo test/netsim/netem.sh on BOARD_IP both loss 2% delay 10ms
#   sudo test/netsim/netem.sh show BOARD_IP
#   sudo test/netsim/netem.sh off BOARD_IP
#
# Direction (optional word after BOARD_IP):
#   out (default): packets this host sends to the board (the audio stream).
#   in: packets the board sends to this host (ACKs, time messages), via an
#       ifb device, because netem only works on outgoing traffic.
#   both: both, each with the same netem settings.
#
# Everything after BOARD_IP is passed to netem (see man tc-netem). The
# interface is the one the host routes BOARD_IP through; set DEV to override.
# "on" again replaces the previous settings. "off" removes everything and
# restores the default qdisc.

set -eu

IFB=${IFB:-netsim-ifb}

usage() {
    echo "usage: $0 on BOARD_IP [out|in|both] NETEM_ARGS... | show BOARD_IP | off BOARD_IP" >&2
    exit 2
}

[ $# -ge 2 ] || usage
cmd=$1
board=$2
shift 2

if [ -z "${DEV:-}" ]; then
    DEV=$(ip route get "$board" | sed -n 's/.* dev \([^ ]*\).*/\1/p' | head -n 1)
fi
[ -n "$DEV" ] || { echo "no route to $board" >&2; exit 1; }

remove() {
    tc qdisc del dev "$DEV" root 2>/dev/null || true
    tc qdisc del dev "$DEV" ingress 2>/dev/null || true
    ip link del "$IFB" 2>/dev/null || true
}

case $cmd in
on)
    DIR=out
    case ${1:-} in out|in|both) DIR=$1; shift ;; esac
    [ $# -ge 1 ] || usage
    remove
    if [ "$DIR" != in ]; then
        # Four bands: all normal traffic goes to band 1 (priomap all 0), only
        # packets to the board are filtered into band 4, which has netem.
        tc qdisc add dev "$DEV" root handle 1: prio bands 4 \
            priomap 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
        tc qdisc add dev "$DEV" parent 1:4 handle 40: netem "$@"
        tc filter add dev "$DEV" parent 1: protocol ip prio 1 u32 \
            match ip dst "$board"/32 flowid 1:4
        echo "$DEV: netem $* for packets to $board"
    fi
    if [ "$DIR" != out ]; then
        # Packets from the board are redirected to the ifb device and leave it
        # through netem; everything else is received normally.
        ip link add "$IFB" type ifb
        ip link set "$IFB" up
        tc qdisc add dev "$IFB" root netem "$@"
        tc qdisc add dev "$DEV" handle ffff: ingress
        tc filter add dev "$DEV" parent ffff: protocol ip prio 1 u32 \
            match ip src "$board"/32 action mirred egress redirect dev "$IFB"
        echo "$DEV: netem $* for packets from $board (via $IFB)"
    fi
    ;;
show)
    tc -s qdisc show dev "$DEV"
    tc filter show dev "$DEV"
    tc filter show dev "$DEV" ingress
    if ip link show "$IFB" >/dev/null 2>&1; then
        tc -s qdisc show dev "$IFB"
    fi
    ;;
off)
    remove
    echo "$DEV: netem off"
    ;;
*)
    usage
    ;;
esac
