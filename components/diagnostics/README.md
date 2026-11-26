# Diagnostics

Optional runtime diagnostics for snapclient. Both are **off by default**; without their option
every call compiles to nothing (sync) or to an empty function (heap), so a normal build is
unchanged. Both use static task stacks and buffers, so they don't take heap away from what they
measure.

| Option | Log tag | What it answers |
|---|---|---|
| `CONFIG_SNAPCLIENT_HEAP_DIAG` | `HEAPDIAG` | How much RAM is free, how fragmented it is, how full the task stacks get |
| `CONFIG_SNAPCLIENT_SYNC_DIAG` | `SYNCDIAG` | Why playback resyncs or reacts late: network, time sync, or the client itself |

Enable them in `idf.py menuconfig` → *Snapclient Diagnostics*, or add the lines to an
`sdkconfig.defaults` file:

```
CONFIG_SNAPCLIENT_HEAP_DIAG=y
CONFIG_SNAPCLIENT_SYNC_DIAG=y
```

Leave the UDP logger (`CONFIG_ESP32_UDP_LOGGER_ENABLED`) off while measuring: it adds its own
buffers and stack use to some tasks.

## Heap diagnostics (`HEAPDIAG`)

Options:

- `CONFIG_SNAPCLIENT_HEAP_DIAG_INTERVAL_S` (default 60): seconds between periodic reports.
- `CONFIG_SNAPCLIENT_HEAP_DIAG_PLAY_LONG_S` (default 600): when the "playback long" report
  is written after playback starts.

A report is a block between `==== <label> (uptime N s, <firmware version>) ====` and
`==== end <label> ====`. It is written at:

| Label | When |
|---|---|
| `boot` | right after the diagnostics started |
| `wifi got ip` / `eth got ip` | the network is up |
| `app_main init done` | startup finished |
| `playback started` / `playback stopped` | the player starts or stops |
| `playback +10 s` | 10 s after playback started |
| `playback long` | `HEAP_DIAG_PLAY_LONG_S` after playback started |
| `periodic` | every `HEAP_DIAG_INTERVAL_S` |

Example (ESP32 without PSRAM, Ethernet, playing FLAC 44.1 kHz with a 700 ms buffer):

```
HEAPDIAG: ==== playback +10 s (uptime 17 s, v0.0.4-12-g1a2b3c4) ====
HEAPDIAG: 8BIT free 39788 largest 34816 min 9996
HEAPDIAG: 32BIT|EXEC free 40048 largest 34816 min 12024
HEAPDIAG: IRAM-only (32BIT minus 8BIT) free 2868
HEAPDIAG: pcm queue 24/26 chunks of 26122 us: 626 ms queued + 46 ms DMA
HEAPDIAG: pcm pool 26 chunks of 4608 B (117 KB): 13 IRAM, 13 DRAM/SPIRAM, min 0 free, 0 dropped
HEAPDIAG: stack heap_diag        hwm  1056 prio  1
HEAPDIAG: stack http             hwm 10744 prio 17
HEAPDIAG: stack player           hwm  1020 prio 20
HEAPDIAG: stack httpd            hwm  5252 prio  5
...
HEAPDIAG: ==== end playback +10 s ====
```

What the lines mean (sizes in bytes):

- **`8BIT free / largest / min`**: heap usable for normal data (DRAM, plus PSRAM if the board has
  it). `free` is free now, `largest` the biggest single block that can still be allocated, `min`
  the lowest `free` since boot. A big gap between `free` and `largest` means the heap is
  fragmented. A low `min` means the board came close to running out at some point.
- **`32BIT|EXEC free / largest / min`**: the same for memory that only allows 32-bit access
  (IRAM). On the ESP32 this also includes the D/IRAM regions that are counted in `8BIT`.
- **`IRAM-only free`** (ESP32 only): the part of the line above that is pure IRAM. PCM chunks are
  placed there first, so 0 while playing means all of it is in use.
- **`pcm queue W/L chunks of U us`**: `W` decoded chunks wait for playback, `L` is the queue
  length, each chunk holds `U` µs of audio. `ms queued` is W × U, `ms DMA` what is already in the
  I2S DMA buffers. Together they are roughly the server buffer (`buf_ms`).
- **`pcm pool N chunks of S B`**: the fixed pool of PCM chunks built when playback starts. How many
  sit in IRAM and how many in DRAM (or PSRAM), the fewest free chunks seen (`min free`), and how
  many decoded chunks had to be dropped because no chunk was free.
- **`stack <task> hwm H prio P`**: `hwm` is the **unused** part of the task's stack at its fullest
  point, in bytes (the high-water mark). Below ~200 is dangerously close to a stack overflow.
  Stack size − hwm = peak use.

## Sync diagnostics (`SYNCDIAG`)

Option: `CONFIG_SNAPCLIENT_SYNC_DIAG_INTERVAL_S` (default 10): seconds per summary.

Every interval the counters are written as three lines and then reset, so each summary covers
only the last interval. In addition, every server settings message (volume, mute, latency, buffer
changes) is logged the moment it arrives.

Example of a normal interval (FLAC 48 kHz, 20 ms chunks, 700 ms buffer, weak Wi-Fi):

```
SYNCDIAG: server settings arrived, lag 4 ms
SYNCDIAG: lag ms chunk 1/4/12 (n 417) settings 4/4/4 time 1/3/6, unsynced 0, gap max 49 ms, rx waits 0 (max 0 ms) after loss 0 (max 0 ms)
SYNCDIAG: time sent 10 replies 10 dup 0 timeouts 0, rtt ms 5/7/10, offset 310043621195 us (moved -1039)
SYNCDIAG: queue min 25, free waits 0 (0 ms, max 0 ms) failed 0, resync init 0 hard1 0 hard2 0 late 0, wifi -69 dBm, disconnects 0 (reason 0), fw v0.0.4-12-g1a2b3c4
```

The same client while the network stalled for ~0.7 s:

```
PLAYER: RESYNCING HARD 2: age 91us, ...
PLAYER: RESYNCING HARD 1: age 9533us, ...
SYNCDIAG: rx stall 690 ms at 312.418 s, then 7300 B in one go (lost segment retransmitted); rtt ms@s before: 7@-0.4 6@-1.4 9@-2.4; rssi/s: -69 -70 -69 -68
SYNCDIAG: lag ms chunk 2/42/663 (n 417) settings 2/4/6 time 2/7/43, unsynced 0, gap max 699 ms, rx waits 1 (max 690 ms) after loss 1 (max 690 ms)
SYNCDIAG: time sent 10 replies 10 dup 0 timeouts 0, rtt ms 6/11/44, offset 310043621722 us (moved 148)
SYNCDIAG: queue min 0, free waits 0 (0 ms, max 0 ms) failed 0, resync init 1 hard1 1 hard2 1 late 0, wifi -69 dBm, disconnects 0 (reason 0), fw v0.0.4-12-g1a2b3c4
```

Values written as `a/b/c` are min/avg/max in ms over the interval; `-` means no samples.

(The `rx stall` line in the second example is illustrative: it shows how a stall caused by a lost
segment looks.)

Where the numbers come from: `main.c` reports each received message and the time sync requests
and replies (`sync_diag_msg()`, `sync_diag_time_*()`), `connection_handler.c` each wait for TCP
data (`sync_diag_rx()`). The player doesn't know about this
component; it reports resyncs and the decoder's waits for a free PCM chunk through
`player_set_event_cb()`, which `sync_diag_init()` registers. So the dependency only goes one way:
diagnostics → lightsnapcast.

### `server settings arrived, lag L ms`

A settings message from the server and how long it took from the server sending it to the client
receiving it. Large values mean the message was stuck on the way (server send queue, network, TCP
retransmits). If the lag is small but the change is still heard late, the delay is in the client.
`(no time sync yet)` is printed before the first time reply; the lag cannot be computed then.

### `rx stall`

`rx stall W ms at T s, then B B[ in one go (lost segment retransmitted)]; rtt ms@s before: R@-S ...; rssi/s: ...`

Logged within a second after the client waited `W` ms (200 or more) for TCP data while it was
ready to take it. A wait caused by the client itself (pool full, so it stopped reading) doesn't
count. `T` is when the wait started (uptime), `B` how many bytes came with the first delivery
after it.

- **in one go**: more than one TCP segment arrived in that delivery. lwIP had them already but
  held them back in its out-of-order queue, waiting for an earlier segment that was lost; the
  delivery came when the server's retransmit filled the hole. Without it, nothing arrived at all
  during the wait (the server didn't send, or everything on the way was held up).
- **rtt before**: the round trips of up to six time replies before the wait, newest first, each
  with how long before the wait it arrived. Round trips that climb to tens or hundreds of ms just
  before a stall point at a growing queue on the way (bufferbloat on a congested router or
  access point). Unchanged round trips and a sudden stall point at the radio (interference,
  roaming) or loss.
- **rssi/s**: the RSSI sampled once per second, from 2 s before the wait until it was logged
  (0 when not on Wi-Fi). A drop during the stall points at the radio link.

At most four stalls are queued per second; more are counted in `rx stall: N more not logged`.

### `time samples since the filter was empty`

Once per connect (or whenever the time filter starts over), the first 48 time samples as one block,
logged when the 48th has arrived (so the logging doesn't delay them):

```
SYNCDIAG: ts     0    3701        0        0 filling
SYNCDIAG: ts    28    3089     -448     -448 filling
...
SYNCDIAG: ts  1543    3009      521      891
```

Columns: arrival in ms since the first sample, round trip in µs, the offset this sample measured
and the filter's estimate after it, both in µs relative to the first sample; `filling` until the
filter counts as full (29 samples, taken 10 ms apart, then one per second). A sample with a long
round trip and a large offset was held up on the way (e.g. a TCP retransmit); it should barely move
the estimate. Playback starts once the filter is full, so a large estimate at that point means a
wrong start.

### Line 1: message lag

`lag ms chunk a/b/c (n N) settings a/b/c time a/b/c, unsynced U, gap max G ms, rx waits W (max X ms) after loss L (max Y ms)`

- **lag**: time from the server sending a message (the `sent` stamp in every message header)
  until the client received it, converted to server time with the time sync. It includes the
  server's send queue, the network, TCP and how late the client read it. Shown per message type:
  - `chunk`: audio chunks, `n` = how many arrived. One chunk per `chunk_ms` on the server
    (e.g. 20 ms → ~500 per 10 s; FLAC sends one per frame, which can be longer).
  - `settings`: server settings messages.
  - `time`: time sync replies.
  A healthy LAN or Wi-Fi shows single-digit ms. Peaks of hundreds of ms are stalls; when they
  get close to the buffer (`buf_ms`), the queue runs empty.
- **unsynced**: messages that arrived before the time sync had an offset, so they have no lag.
  Normal only in the first interval after connecting.
- **gap max**: longest time between two arriving messages of any type. While audio plays a
  chunk arrives every few ms, so a gap of several hundred ms is a stall where nothing arrived at
  all. It also counts gaps the client caused itself: when the PCM queue is full, the client stops
  reading and the server's sending stalls on the full TCP window.
- **rx waits**: how often the client waited 100 ms or more for TCP data while it was ready to
  take it, and the longest such wait. Unlike `gap max`, this leaves out the client's own pauses.
- **after loss**: waits of 20 ms or more that ended with several TCP segments arriving at once,
  i.e. the wait was for the retransmit of a lost segment (see `rx stall`), and the longest of
  them. Many of them with short waits are normal TCP recovery; long ones empty the queue.

### Line 2: time sync

`time sent S replies R dup D timeouts T, rtt ms a/b/c, offset O us (moved M)`

- **sent / replies**: time requests the client sent and replies it got. During the first seconds
  after connecting the client sends them quickly (fast sync), later about one per second.
- **dup**: replies that arrived in the same TCP segment as the previous one (same arrival
  time). They were held up on the way, so they are not used for the clock offset. Many
  duplicates mean bursts on the network.
- **timeouts**: times when requests were still unanswered after 500 ms and the client gave up
  waiting for them. Should be 0.
- **rtt**: round trip of the time replies, i.e. server processing time plus the client's
  receive-to-handle time. This is what the clock offset accuracy depends on.
- **offset**: current difference between the client's clock and the server's clock, in µs. The
  absolute value means nothing (the two clocks start at different times). **moved** is how far it
  changed since the last summary; small values (a few hundred µs to ~1 ms per 10 s) are normal
  clock drift. Jumps of several ms mean the time sync got bad samples.

### Line 3: playback, resyncs, Wi-Fi

`queue min Q, free waits W (T ms, max X ms) failed F, resync init I hard1 H1 hard2 H2 late L, wifi R dBm, disconnects D (reason C), fw V`

- **queue min**: fewest decoded chunks waiting for playback, sampled whenever an audio chunk
  arrives. In steady state it is close to `buf_ms / chunk duration`. 0 means the queue ran empty:
  the player had nothing to play and has to resync. `-1` means no audio chunk arrived.
- **free waits**: how often the decoder had to wait for a free PCM chunk (only waits of 1 ms or
  more are counted), total and longest wait. A wait means the queue was full, i.e. the client is
  ahead and the network is fine. **failed**: the wait (up to 100 ms) ran out and the chunk was
  dropped.
- **resync**:
  - `init`: playback (re)started and the first chunk was synced to its play time. One per
    playback start; more mean the player restarted.
  - `hard1`: during that start, the first chunk was already too late to be played, so it was
    dropped and the start retried.
  - `hard2`: while playing, the queue ran empty or the audio drifted more than 2 ms from where it
    should be. Playback stops and starts again (an audible gap).
  - `late`: wire chunks skipped without decoding because their play time had already passed.
    They arrive like that after a stall, as the backlog drains; skipping them saves the decoding
    and one `hard1` each.
  The `PLAYER: RESYNCING HARD 1/2` and `late wire chunk skipped` warnings are logged at the same
  time with more detail, at most one line per second.
- **wifi**: signal strength of the current access point (0 on Ethernet or when not connected).
  Below about −75 dBm, retransmits and stalls become common.
- **disconnects**: Wi-Fi disconnects in this interval and the reason code of the last one
  (`wifi_err_reason_t` in ESP-IDF, e.g. 200 = beacon timeout, 201 = AP not found,
  15 = 4-way handshake timeout).
- **fw**: firmware version (`git describe`; the part after `-g` is the commit)

### Reading the result

| What you see | Likely cause |
|---|---|
| `gap max` and `chunk` lag peaks of hundreds of ms, then `queue min 0`, `hard2` and `late` | Network stalls longer than the buffer can bridge. Check `wifi` dBm and `disconnects`; a larger `buf_ms` or better Wi-Fi helps. |
| `settings` lag high while `chunk` lag is low | The settings message was held up on the server side. |
| `settings` lag low, but the change takes effect late | Delay inside the client. |
| `timeouts` or many `dup` | Time replies lost or arriving in bursts; the clock offset gets worse. |
| `offset moved` by several ms between summaries | Time sync got bad samples, so playback is shifted; expect `hard2`. |
| `free waits ... failed` > 0 | Pool too small for the server buffer (see `HEAPDIAG` `pcm pool`). |
| `hard2` while `queue min` stays high | Drift, not starvation: look at the time sync line. |
