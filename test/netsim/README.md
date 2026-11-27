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

The proxy holds at most 256 KB per direction (`--max-buffer KB`, 0 = unlimited). When a client
reads slower than the server sends, the proxy stops reading and the server sees a full TCP window,
as on a real link, so snapserver drops old chunks. Without the limit the proxy queued minutes of
audio for a client that fell behind, and its time-sync replies waited behind that backlog.
`--stats 1` logs the bytes queued per direction every second.

## Setup

```
board  --Wi-Fi-->  this PC :1706 (netsim_proxy.py)  -->  snapserver :1704 (testserver.sh)
```

Needs Python 3, `snapserver` and `ffmpeg` on this PC. The test server uses the default snapserver
ports (1704, 1705, 1780): stop a system snapserver first, or move the test server with
`STREAM_PORT`, `TCP_PORT` and `HTTP_PORT` (then pass `--target` to the proxy and `--server` to
`client.py`). All commands from the repository root.

1. Start the test server with the server buffer you want (here 750 ms):
   ```sh
   test/netsim/testserver.sh 750 &
   ```
   It plays test tones on four streams: `flac44`, `flac48`, `opus`, `pcm`. It is not announced via
   mDNS, so the board only reaches it through the proxy. To change the buffer, stop it and start
   it again.
2. Start the proxy:
   ```sh
   python3 test/netsim/netsim_proxy.py --listen 1706 --control /tmp/netsim.ctl
   ```
3. Point the board at the proxy (once; the setting survives flashing). This turns mDNS off on the
   board and restarts it:
   ```sh
   test/netsim/client.py point BOARD PC_IP
   ```
4. Start logging the board's serial output, then pick the stream. This also sets the volume to
   1 %, because a muted client doesn't start its player:
   ```sh
   test/netsim/client.py stream flac44
   ```
   Check the stream in the log: the PCM pool line shows the chunk size (FLAC 44.1 kHz: 4608 B).
   `client.py status` lists the server's clients.
5. Send commands to the proxy:
   ```sh
   echo "stall 2.5" > /tmp/netsim.ctl               # one 2.5 s outage
   echo "jitter 0.1 0.7 3 1" > /tmp/netsim.ctl      # random 0.1-0.7 s stalls, every ~3 s
   echo "jitter off" > /tmp/netsim.ctl
   ```
   Or run unattended with `--schedule "60 jitter 0.1 0.7 3 1" "300 jitter off" "330 quit"`.
   Packet loss can be added at the same time (see below).
6. Afterwards: stop the proxy and the test server, `netem.sh off` if used, and
   `test/netsim/client.py restore BOARD` to give the board its default server settings back.

The jitter sequence depends only on the seed, so two firmware builds see exactly the same stalls.

## Packet loss (netem)

`netem.sh` drops and delays packets between this host and the board, using the kernel's netem
qdisc. It needs root. Other traffic on the interface is not touched.

```sh
sudo test/netsim/netem.sh on BOARD loss 2% delay 30ms 10ms            # to the board only
sudo test/netsim/netem.sh on BOARD both loss 2% delay 10ms           # both directions
sudo test/netsim/netem.sh show BOARD                                  # counters: sent, dropped
sudo test/netsim/netem.sh off BOARD                                   # remove everything
```

The optional word after `BOARD` selects the direction:
- `out` (default): packets to the board. That is the audio stream: lost segments are retransmitted
  by this host, and the board holds the segments after a gap in its pbufs.
- `in`: packets from the board (ACKs, Hello, time messages). netem only works on outgoing traffic,
  so these are redirected to an `ifb` device (`netsim-ifb`) and dropped there. Lost ACKs mostly
  make the server's window open later and in bigger steps. Lost time messages and other data
  make the board's lwIP retransmit, which holds pbufs and Wi-Fi TX buffers. Lost or delayed time
  messages also skew the latency measurement.
- `both`: the same netem settings on both sides, which is closest to bad Wi-Fi. Delay is then
  added twice to the round trip (`delay 10ms` gives +20 ms RTT).

`on` again replaces all previous settings (both sides).

Here the TCP connection to the board starts on this host: at the proxy, or at the snapserver
when the board connects to it directly. So a dropped segment is really lost. This host's TCP
retransmits it, backs off and restarts slow start. Meanwhile the board holds the segments after
the gap in its pbufs. The proxy's stalls and netem can run at the same time.

Notes:
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

## Scripted runs

`run_test.sh` does a whole run unattended: test server, optional flashing, serial log, pointing
the board at this PC, starting the stream, and `MARK <phase>` lines in the log.

```sh
# 5 min straight to the test server; set the loss with netem.sh first (sudo)
BOARD=<board IP> PC=<this PC's IP> TTY=/dev/serial/by-id/<board> \
  test/netsim/run_test.sh loss run1.log
# through the proxy: stalls of 1.0, 2.5 and 4.0 s, then 120 s of jitter 0.1 0.7 3 1
BOARD=... PC=... TTY=... FW=build test/netsim/run_test.sh stalls run2.log
```

`FW=<IDF build dir>` flashes that build first (`flash.sh`, esptool from the IDF Docker image at
1 Mbaud). `BUF`, `STREAM`, `DURATION` (loss run, s) and `CLIENT` change the defaults (750 ms,
`flac44`, 300 s, the connected ESP32). Use the `/dev/serial/by-id/` path for `TTY`: `ttyUSBn`
numbers change when boards are replugged. `serial_log.py PORT LOG` logs on its own; it keeps DTR
and RTS off, so opening the port doesn't reset the board. The board stays pointed at this PC:
`client.py restore BOARD` afterwards. Server output goes to `LOG.server`, the proxy's to
`LOG.proxy`.

## Analysis: client, server TCP, packets

Client side, from a build with `CONFIG_SNAPCLIENT_SYNC_DIAG` (see
[components/diagnostics](../../components/diagnostics/README.md)):

```sh
python3 test/netsim/syncdiag_summary.py run1.log [run2.log ...]
```

Per phase: resyncs (init, HARD 1, HARD 2, late chunks skipped), queue minimum, silence after each
HARD 2, waits for TCP data and the `rx stall` lines (each wait >= 200 ms, and whether it ended with
a retransmitted segment).

Server side, on the server host during the run (no root needed):

```sh
python3 test/netsim/ss_sample.py ss1.txt 360 <board IP> &   # ss -ti every 20 ms for 360 s
python3 test/netsim/ss_summary.py ss1.txt
```

It shows the congestion window, retransmit timeout and backoff, retransmits, and how long the
server waited for the client's ACKs.

Packets, on the server host (needs root):

```sh
sudo tcpdump -i <iface> -s 128 -w cap.pcap host <board IP> and tcp port 1704
python3 test/netsim/pcap_tcp.py cap.pcap
```

Per connection: lost segments and their recovery time, spurious retransmits (the server's timer
fired while it waited for an ACK), and each pause >= 150 ms in the server's sending sorted by
cause: delayed ACK from the client, tail loss (retransmit timeout), or idle. On the server host
sent packets are captured after netem's drop and received ones before netem's ingress drop. To
run tcpdump in the background, start it with `sudo -v` first, and with `setsid nohup` over SSH.

Findings so far with 2 % loss both ways (LyraT, FLAC 44.1, 750 ms, #261): the server's congestion
window falls to 3-4 segments, so a lost last segment of a chunk waits for Linux's retransmit
timeout (min 200 ms) and a lost retransmit doubles it (0.7-1 s, HARD 2). The client's delayed ACK
(250 ms) made the server's timer fire on its own; the client now ACKs every read segment
(`TCP_WND_UPDATE_THRESHOLD` in the top-level CMakeLists.txt): pauses 66 -> 12 per 5 min. On the
server host, `ip route change <subnet> dev <iface> rto_min 50ms` or
`sysctl net.ipv4.tcp_thin_linear_timeouts=1` removed the long stalls (see TODO.md).

## CI

`test_netsim_proxy.py` tests the proxy itself on the host (pass-through, stall and burst, rate
limit, buffer limit) and runs in the `netsim` workflow. A hardware-in-the-loop job would run the scenario above
against a board on the runner's network and fail on hard resyncs or errors from `--counts`.
