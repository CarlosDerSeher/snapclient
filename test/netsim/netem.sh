#!/bin/sh
# Packet loss and delay towards one board, with Linux netem on this host.
# Needs root (tc). Only packets this host sends to BOARD_IP are affected.
#
#   sudo test/netsim/netem.sh on BOARD_IP loss 2% delay 30ms 10ms
#   sudo test/netsim/netem.sh show BOARD_IP
#   sudo test/netsim/netem.sh off BOARD_IP
#
# Everything after BOARD_IP is passed to netem (see man tc-netem). The
# interface is the one the host routes BOARD_IP through; set DEV to override.
# "on" again replaces the previous settings. "off" restores the default qdisc.

set -eu

usage() {
    echo "usage: $0 on BOARD_IP NETEM_ARGS... | show BOARD_IP | off BOARD_IP" >&2
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

case $cmd in
on)
    [ $# -ge 1 ] || usage
    tc qdisc del dev "$DEV" root 2>/dev/null || true
    # Four bands: all normal traffic goes to band 1 (priomap all 0), only
    # packets to the board are filtered into band 4, which has netem.
    tc qdisc add dev "$DEV" root handle 1: prio bands 4 \
        priomap 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
    tc qdisc add dev "$DEV" parent 1:4 handle 40: netem "$@"
    tc filter add dev "$DEV" parent 1: protocol ip prio 1 u32 \
        match ip dst "$board"/32 flowid 1:4
    echo "$DEV: netem $* for packets to $board"
    ;;
show)
    tc -s qdisc show dev "$DEV"
    tc filter show dev "$DEV"
    ;;
off)
    tc qdisc del dev "$DEV" root 2>/dev/null || true
    echo "$DEV: netem off"
    ;;
*)
    usage
    ;;
esac
