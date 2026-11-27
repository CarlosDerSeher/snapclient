#!/bin/bash
# One scripted netsim run: start the test server, optionally flash, log the
# serial output, point the board at the server, start a stream, mark the
# phases in the log, clean up.
#
#   test/netsim/run_test.sh loss LOG     straight to the server; set netem loss
#                                        yourself first (netem.sh, needs sudo)
#   test/netsim/run_test.sh stalls LOG   through netsim_proxy.py: stalls of 1.0,
#                                        2.5 and 4.0 s, then 120 s of jitter
#
# Environment (BOARD, PC and TTY are required):
#   BOARD    board IP             PC       this PC's IP as the board sees it
#   TTY      serial port (use /dev/serial/by-id/...)
#   CLIENT   snapcast client ID, name or IP (default: the connected ESP32)
#   FW       IDF build directory to flash first (default: don't flash)
#   BUF      server buffer in ms (default 750)   STREAM  stream (default flac44)
#   DURATION length of the loss run in s (default 300)
#
# The log gets "MARK <phase>" lines (play, end; stall 1.0, ... for stalls) that
# syncdiag_summary.py splits on. The board stays pointed at this PC afterwards:
# restore it with test/netsim/client.py restore BOARD. Run from the repository root.
set -u
MODE=$1; L=$2; D=$(dirname "$0")
: "${BOARD:?}" "${PC:?}" "${TTY:?}"
BUF=${BUF:-750}; STREAM=${STREAM:-flac44}; DURATION=${DURATION:-300}
CL=(); [ -n "${CLIENT:-}" ] && CL=(--client "$CLIENT")
mark() { echo "$(date +%T) MARK $*" >> "$L"; }

sh "$D/testserver.sh" "$BUF" > "$L.server" 2>&1 & SRV=$!; sleep 3
PORT=1704
if [ "$MODE" = stalls ]; then
  C=$(mktemp); PORT=1706
  python3 "$D/netsim_proxy.py" --listen 1706 --target 127.0.0.1:1704 --control "$C" > "$L.proxy" 2>&1 & PX=$!
fi
[ -n "${FW:-}" ] && TTY=$TTY sh "$D/flash.sh" "$FW" | tail -1
python3 "$D/serial_log.py" "$TTY" "$L" & LP=$!
# point restarts the board; playing = an "initial sync" in the log since then
sleep 25; m=$(wc -l < "$L"); python3 "$D/client.py" point "$BOARD" "$PC" $PORT > /dev/null; sleep 20
for try in 1 2 3 4; do
  python3 "$D/client.py" stream "$STREAM" "${CL[@]}" --volume 1 --settle 1 > /dev/null 2>&1; sleep 8
  tail -n +"$m" "$L" | grep -a -q "initial sync" && break
  echo "no initial sync yet, starting the stream again"
done
if [ "$MODE" = stalls ]; then
  sleep 20
  for s in 1.0 2.5 4.0; do mark stall $s; echo "stall $s" > "$C"; sleep 45; done
  mark jitter; echo "jitter 0.1 0.7 3 1" > "$C"; sleep 120; echo "jitter off" > "$C"
  mark end; sleep 12
else
  mark play; sleep "$DURATION"; mark end; sleep 2
fi
kill $LP
if [ "$MODE" = stalls ]; then echo quit > "$C"; sleep 1; kill $PX 2>/dev/null; rm -f "$C"; fi
# the test server and the ffmpeg tone generators it started
tree() { echo "$1"; for c in $(pgrep -P "$1"); do tree "$c"; done; }
kill $(tree $SRV) 2>/dev/null
exit 0
