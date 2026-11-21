# netsim: bad Wi-Fi between snapclient and snapserver

`netsim_proxy.py` is a TCP proxy you put between a board and a snapserver to test how the client
copes with Wi-Fi outages, reproducibly. Only Python 3 is needed; no root, no `tc`.

While a stall is active, the proxy keeps reading from both sides but holds the data. When the stall
ends, the backlog goes out in one burst. To the client this looks like a Wi-Fi outage: no audio
chunks and no time-sync replies, then everything at once at the speed of the real link. That burst
is what stresses the client's TCP window, Wi-Fi RX buffers and PCM queue.

What it does **not** simulate: the proxy ends TCP on both sides, so nothing is ever lost. The
PC's TCP stack ACKs every segment, and there are no retransmissions, retransmission-timeout
backoff, slow start after a timeout, or out-of-order segments held by the client. After real loss
the backlog arrives later and slower than after a proxy stall, so recovery times measured here are
optimistic, and the client's pbufs and Wi-Fi RX buffers are stressed less. For loss and extra
delay, add netem on the same host (see [Packet loss](#packet-loss-netem)).

## Setup

```
board  --Wi-Fi-->  this PC :1706 (netsim_proxy.py)  -->  snapserver :1704
```

1. Run a snapserver (the proxy defaults to `127.0.0.1:1704`). Use one that isn't published via
   mDNS, or turn mDNS off on the board, so the board doesn't connect around the proxy.
2. Start the proxy:
   ```sh
   python3 test/netsim/netsim_proxy.py --listen 1706 --target 127.0.0.1:1704 --control /tmp/netsim.ctl
   ```
3. Point the board at it (web UI, or the same via HTTP), then restart it:
   ```sh
   curl -X POST "http://BOARD/post?param=snapserver_host&value=PC_IP"
   curl -X POST "http://BOARD/post?param=snapserver_port&value=1706"
   curl -X POST "http://BOARD/post?param=snapserver_use_mdns&value=0"
   curl -X POST "http://BOARD/restart"
   ```
   `curl -X DELETE "http://BOARD/delete?param=snapserver_host"` (and `_port`, `_use_mdns`)
   restores the defaults afterwards.
4. Send commands:
   ```sh
   echo "stall 2.5" > /tmp/netsim.ctl               # one 2.5 s outage
   echo "jitter 0.1 0.7 3 1" > /tmp/netsim.ctl      # random 0.1-0.7 s stalls, every ~3 s
   echo "jitter off" > /tmp/netsim.ctl
   ```
   Or run unattended with `--schedule "60 jitter 0.1 0.7 3 1" "300 jitter off" "330 quit"`.

The jitter sequence depends only on the seed, so two firmware builds see exactly the same stalls.

## Packet loss (netem)

`netem.sh` drops and delays packets that this host sends to the board, using the kernel's netem
qdisc. It needs root. Other traffic on the interface is not touched.

```sh
sudo test/netsim/netem.sh on BOARD loss 2% delay 30ms 10ms   # start, or change settings
sudo test/netsim/netem.sh show BOARD                         # counters: sent, dropped
sudo test/netsim/netem.sh off BOARD                          # restore
```

Here the TCP connection to the board starts on this host: at the proxy, or at the snapserver
when the board connects to it directly. So a dropped segment is really lost. This host's TCP
retransmits it, backs off and restarts slow start. Meanwhile the board holds the segments after
the gap in its pbufs. The proxy's stalls and netem can run at the same time.

Notes:
- Only the direction host → board is affected. That is the audio stream, and it is what matters
  for a client that mostly receives. Loss towards the host (the board's ACKs and time messages)
  would need an `ifb` device.
- Use the real interface the board is reached through (the script asks `ip route get`; set `DEV`
  to override). Loopback won't do.
- Delay with jitter (`delay 30ms 10ms`) also reorders packets. Add `rate` or leave out the jitter
  if you don't want that.
- netem's random drops are not seeded with iproute2 < 6.7, so runs differ. Compare builds over
  several runs, or use a `loss gemodel` burst pattern.
- Always run `off` afterwards. The settings stay until then, or until a reboot.

Typical settings: `loss 1%`, `loss 3%`, burst loss `loss gemodel 1% 30% 70% 0.1%`, delay
`delay 30ms 10ms`.

## Reading the result

Log the board's serial output during the run, then:

```sh
python3 test/netsim/resync_summary.py board.log            # timeline
python3 test/netsim/resync_summary.py board.log --counts   # totals
```

With `CONFIG_SNAPCLIENT_HEAP_DIAG` on, the log also has the minimum free heap and the PCM pool's
drop counter. The proxy logs every stall and the size of each burst with a wall-clock timestamp.

Reference scenario used for the RAM work: FLAC 44.1 kHz, server buffer 750 ms, 60 s settle, then
240 s of `jitter 0.1 0.7 3 1` (73 stalls), then 20 s calm.

## CI

`test_netsim_proxy.py` tests the proxy itself on the host (pass-through, stall and burst, rate
limit) and runs in the `netsim` workflow. A hardware-in-the-loop job would run the scenario above
against a board on the runner's network and fail on hard resyncs or errors from `--counts`.
