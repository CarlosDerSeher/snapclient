# TODO

Open work on this project, roughly in priority order within each section. Pick something, open an
issue or a draft PR to say you are on it, and target `experimental`, which collects the recent
changes until they are merged into `develop` together.

Items marked *in progress* have an open PR; check it before starting.

## 1. RAM reduction (ESP32 without PSRAM)

Goal: run a plain ESP32 without PSRAM, built with `sdkconfig.defaults`, with a server buffer
(`buf_ms`) of up to ~750 ms. Every KB saved becomes PCM buffer space.

<details>
<summary>Ground rules for this work</summary>

- Reference stream: FLAC, 44.1 kHz, 16 bit, 2 ch. Snapserver uses a fixed FLAC blocksize of 1152
  frames, so one chunk is 4608 B of PCM (~26 ms).
- `buf_ms` is set on the server and unknown at build time: no build-time buffer budget in Kconfig.
- No heap allocation in the audio path at runtime (decode → queue → playback). Variable-size
  allocations fragment the heap.
- PCM chunks may live in IRAM: access those buffers only with aligned 32-bit reads/writes.
- `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y` stays. ESP32 + PSRAM boards are tight on IRAM, so changes to
  `sdkconfig.defaults*` or new IRAM code must build for all ESP32 PlatformIO envs.
- One item per PR. Measure before and after with `CONFIG_SNAPCLIENT_HEAP_DIAG` (free / largest
  block / minimum free heap, stack high-water marks).

</details>

Done and merged into `experimental`: boot transients and IRAM → flash (#247), web OTA instead of
the standalone OTA server (#249), UI task removed and DSP JSON streamed (#252), task stacks trimmed
(#254), Opus optional (#255), one wire-chunk buffer per codec (#256), fixed PCM chunk pool (#258),
Wi-Fi / lwIP tuning (#259), 1 KB libFLAC input buffer (#257).

### 1.1 Task stacks
Done for `http` (15 → 13 KB, 4 KB without Opus) and `httpd` (8 → 6 KB) in #254/#255. Left:

- With Ethernet: `playback_monitor_task` (4 KB, permanent) and `static_ip_task` (4 KB, created at
  runtime) → event handler or `esp_timer`. `eth_on_playback_stopped()` can block for up to 2.6 s, so
  it cannot simply run as an `esp_timer` callback.

### 1.2 FLAC input without the wire-chunk buffer
Done differently in #256: the parser stays the only reader and reads each wire chunk into one
buffer allocated per codec header for the worst case (FLAC 4672 B, Opus 1276 B, PCM none), instead
of a `malloc` per chunk. Reading straight from the connection in `read_callback` was tried and
dropped.

### 1.3 Fixed PCM chunk pool
Done in #258 (includes the `player_task` exit fix from the closed #253). Replaced the per-chunk
`malloc` and the unused fragmented-chunk code with a fixed pool. Mainly a robustness change: no
fragmentation, no "malloc failed, retry" loops.

Changed from the design below after measuring: no spare chunks (N = queue formula), the decoder
waits up to 100 ms for a free chunk instead of dropping audio, DRAM reserve 32 KB, the pool is
built again once more RAM is free. Largest buffer FLAC 44.1 kHz: 850 ms (old code ~950 ms with
4–7 KB left).

<details>
<summary>Design</summary>

- **Slots**: N equal-size buffers, IRAM first (`MALLOC_CAP_32BIT | MALLOC_CAP_EXEC`) until that
  fails, then DRAM. Slot size = max blocksize × channels × bytes per sample.
- **Descriptors** (timestamp, size, payload pointer) in a static DRAM array; queues of descriptor
  pointers: `freeQ` (empty) and the existing PCM queue (filled). Decoder takes from `freeQ`, the
  player gives back. Blocking replaces the retry loops.
- **N** = current queue length formula + 2 (one being decoded, one being played).
- **Build** the pool in the decode path as soon as `buf_ms` and the chunk size are known (FLAC:
  STREAMINFO in `metadata_callback`; Opus: `opus_packet_get_samples_per_frame()` on the first
  packet; PCM: first chunk). Keep it across pause/reconnect; rebuild only when `buf_ms` or the slot
  size changes.
- **Failure**: if N slots don't fit, don't play with fewer. Log clearly ("server buffer 1000 ms
  needs 175 KB, available 140 KB — see README") and stay stopped. Keep a DRAM reserve (~20–30 KB,
  tune by measurement) for Wi-Fi, lwIP and httpd.
- **Decoders write directly into slots**: FLAC `write_callback` writes packed 32-bit frames;
  Opus decodes into one static scratch buffer, then a 32-bit copy; PCM as today.
- Remove `pcm_chunk_fragment_t` and all fragment handling.

Acceptance: no `malloc/realloc/free` from decode to playback after the pool is built; minimum free
heap and largest free block flat over 10 min of playback; sync unchanged across codec switches,
buffer changes on the server and reconnects.

</details>

### 1.4 Wi-Fi / lwIP tuning
Done in #259. Tested with simulated outages and real packet loss
(`test/netsim/`: TCP proxy for outages, `netem.sh` for loss in either direction, test server and
board control scripts; `test/maxbuf/` for the largest server buffer).

- Dynamic Wi-Fi TX buffers, `ESP_WIFI_RX_BA_WIN` 16 → 6, static RX buffers 8 → 6, TCP send buffer
  11680 → 5840, no SoftAP, no Enterprise: +17 KB free heap after Wi-Fi start, −50 KB flash,
  resyncs unchanged under outages and loss.
- The TCP receive window stays at 11680. With 5840 (or 8760) the server is held back by the window
  after packet loss and the queue takes seconds to refill; the smaller window would have allowed
  925 instead of 850 ms server buffer.
- IPv6 off doesn't compile (IPv6 code without guards in the Ethernet and connection code).
- `LWIP_MAX_UDP_PCBS` has no effect: IDF's lwIP allocates control blocks from the heap and only
  counts TCP ones. Dropped from the defaults.

### 1.5 Optional: buffer compressed FLAC
Only if the above is not enough. Saves ~40–60 KB at 750 ms, but only with a fixed ring buffer and a
small PCM look-ahead decoded just in time.

### 1.6 TAS5805M without PSRAM
All TAS5805M boards have PSRAM today. The driver itself needs ~1 KB static RAM plus a 3 KB fault
task; the blocker is the web UI, which builds large cJSON trees (and 1–64 KB buffers) for the DAC/EQ
settings and schemas. Needs a TAS5805M board to test.

- Measure the JSON size of `/api/dac/settings`, `/api/dac/schema`, `/api/eq/settings` and
  `/api/eq/schema`.
- Serve the schema (static per build) as a file from flash; generate only the current values and
  stream them like the DSP JSON (#252).
- POST: one parameter per request, or a small parser instead of `cJSON_Parse()` on the whole body.
- Fault task (3 KB, polling) → periodic job without its own stack.

### 1.7 Minimum-RAM sdkconfig
A defaults file for a plain ESP32 without PSRAM that collects everything that saves RAM, instead of
relying on `sdkconfig.defaults`.

- DSP off, Opus optional (`CONFIG_SNAPCLIENT_DISABLE_OPUS`, #255). The UDP logger is already off
  in `sdkconfig.defaults` (see 1.8).
- The Wi-Fi / lwIP settings from 1.4 once they are tested.
- A PlatformIO env for it, and the README "Boards without PSRAM" table measured with it.

### 1.8 Lighter UDP logger
Off by default in `sdkconfig.defaults` for now (its Kconfig defaults to `y`). It costs a 32-line
queue (32 × 258 B ≈ 8 KB), two tasks, and ~0.5 KB of stack in every task that logs: `udp_vprintf()`
formats into a 256 B stack buffer and `queue_line()` copies it into a second 256 B `log_item_t` on
the stack. With it on, #261 saw stack overflows in `esp_timer` (Improv shutdown) and `mdns` (the
W5500 IPv6 multicast warning), and the S3 player task had 160 B left.

- Format straight into the queue item, or into one static buffer under a mutex: ~0 B extra stack
  per logging task.
- Smaller queue / line length as Kconfig options with lower defaults; drop lines when full.
- Then consider turning it back on by default, build-checked against all envs.

## 2. Robustness and bugs

- Ethernet boot delay: `esp_eth_start()` blocks `app_main` for 4 s in ~1 of 3 resets when no cable
  is plugged in (auto-negotiation timeout). Lower `autonego_timeout_ms` in
  `components/network_interface/eth_interface.c`, then check boot timing and that a plugged-in cable
  still gets link and IP.
- Hard resyncs: #117, #191, #226.
- Hard resyncs at startup under packet loss: playback starts with a server time offset that is off
  by up to ~150 ms, then hard-resyncs every 2.6 s (up to 4 times) while the estimate converges.
  Seen in every run with 2 % loss (`test/netsim/netem.sh`), never without loss. Likely cause: time
  replies share the TCP connection with the audio, so a retransmission delays them by 200 ms or more
  (offset error ≈ delay / 2). Time replies are now stamped when they arrive instead of when they
  are parsed (see Sync between boards), which removes the decode delay but not a retransmission. The time filter counts as full after 29 samples taken 10 ms apart
  during the startup burst, takes the first sample as the offset and the first two as the drift,
  and then samples only once per second. Confirm by logging each sample (round trip, offset, filter
  estimate) for the first seconds. Possible fixes:
  - count the filter as full only after a minimum time span (e.g. 2–3 s of fast sync), not after
    29 samples;
  - ignore samples whose round trip is far above the lowest seen so far (as NTP does);
  - start from the sample with the shortest round trip instead of the first one.
- Sync between boards. Measured with `test/sync/` (branch `sync-measure`, not merged yet): two
  boards' analog outputs into the PC's Line In, offset per 0.5 s window from cross-correlating a
  pink-noise stream. LyraT 4.3 vs ADAU1961 board, 200 s each:
  - Fixed: time replies were stamped when parsed, behind up to ~10 ms of audio decoding; half of
    that went into the server time offset. Now stamped when the TCP segment arrives. Offset between
    the boards std 179 → 68 µs, jumps > 200 µs 15 → 5, no early hard resyncs.
  - Fixed: after a reconnect the state stayed IDLE, so the codec stayed stopped (ADAU1961 silent)
    and Wi-Fi power save stayed on during playback.
  - Left: a constant ~400 µs offset (ADAU1961 later), probably the codecs' different output
    latency. The client latency setting only takes whole milliseconds. Now compensated per DAC
    with `CONFIG_DAC_GROUP_DELAY_FRAMES` (frames, converted with the current sample rate), but only
    the ADAU1961 has a value so far (25, datasheet); see "DAC group delays" below.
  - Left: the control loop corrects only when the median error passes ±128 µs (`SHORT_OFFSET`),
    then inserts or drops one sample per chunk, which shows up as ~230 µs steps between boards.
    Smaller now that the time base is clean.
  - Left: the time filter's drift estimate stays 0, while the board crystals drift 3.7–5.8 ppm
    against the server; the offset is only followed, never predicted.
  - Left: the LyraT's time round trip is ~5 ms (minimum 1.7 ms) even with arrival stamps, so its
    delay is before the board (Wi-Fi, TCP or server). Needs a packet capture.
- DAC group delays: `CONFIG_DAC_GROUP_DELAY_FRAMES` defaults to 0 for every DAC except the
  ADAU1961 (25 frames = 567 µs at 44.1 kHz). Find the delay of the others, from the datasheet's
  interpolation filter table where it is given, otherwise by measurement with `test/sync/` against
  the ADAU1961 board as reference (same firmware and DSP settings on both, swap the Line In plugs
  for a second run to cancel the sound card's channel skew). Then add a `default` per DAC in
  `components/audio_board/Kconfig.projbuild`:
  - ES8388 (LyraT 4.2/4.3, AI-Thinker): not in the datasheet (rev. 5.0). First measurement
    (44.1 kHz, ADAU1961 with its 25 frames as reference, one plug orientation): LyraT ~336 µs
    later, so roughly 15 frames. Swap the plugs to cancel the sound card skew before using it.
  - PCM5102A (PCM5100A/5101A): 20/fS normal filter, 3.5/fS low latency (datasheet table; the
    filter section says 22/fS), check by measurement. PCM5121/5122: 20/fS.
  - TAS57xx (TAS5754M/56M): probably the PCM51xx filters (20/fS), not confirmed.
  - TAS5805M: no datasheet value; TI (E2E) says the delay comes from the DSP (EQ, SRC) and depends
    on the configuration (0-127 samples for the FIR). Measure with our default EQ.
  - MA120 / MA120X0, MAX98357, PT8211: no datasheet value. PT8211 has no oversampling filter, so
    probably ~0.
  - The DACs of the other ESP-ADF boards (LyraTD-MSC, LyraT-Mini, Korvo DU1906, Kaluga).
  - ADAU1961 at 88.2/96 kHz: the driver leaves R17 at its default (CONVSR = base rate), so the
    chip runs 96 kHz as its base rate in normal mode. The datasheet's 11/fS is for the 96 kHz mode
    (CONVSR = fS/0.5 from a 48 kHz base); whether our setup is in spec and what its delay is needs
    a measurement.
- Sync drift fixed (experimental): the control loop took the play time from the written sample
  count at the nominal rate, but the I2S clock divider runs ~5.5 ppm slow at 44.1 kHz (same on
  every ESP32, measured with the DMA on_sent callback), and underruns (zeros from `auto_clear`)
  were not seen at all. Boards drifted ~0.33 ms/min later without a resync, 141 ms after a night.
  Now the play time comes from the DMA callbacks (last descriptor time, underrun count). Check
  48 kHz and APLL builds (`USE_SAMPLE_INSERTION` off) too, and a long run (several hours).
- ADAU1961 with 24-bit streams: for more than 16 bits the player sets the I2S MCLK to 384 × fs
  (`player_setup_i2s()`), but the ADAU1961 driver always sets R0 INFREQ to 256 × fs, so the codec
  would take 1.5 × the stream rate as its base rate. INFREQ has no 384 × fs setting (256, 512,
  768 or 1024 × fs), so for this codec use MCLK 768 × fs with INFREQ = 10 (an integer BCLK divider
  for 24-bit slots too), or the PLL. Not tested yet; check with a 24-bit test stream.
- LyraT 4.3 with ESP32 rev v1.0 crashes at boot in ~1 of 5 boots: heap corruption shortly after
  got-IP, seen in `udp_sendto()` from the UDP logger and, with the logger off, in `heap_caps_free()`
  inside the mDNS server lookup. A LyraT with rev v3.0 and the ADAU1961 board (no PSRAM) don't show
  it. Check the PSRAM cache workaround for rev v1 and what touches the heap around got-IP.
  The same rev v1.0 board also answers pings only after 1–9 s for minutes after most restarts, so
  its clock sync stalls; that happens with every firmware variant tried, while the same image on
  the rev v3.0 LyraT boots clean. Possibly a faulty board; try a second rev v1 board before
  spending more time on it.
- Channel order differs between codecs: FLAC packs a stereo frame as `L | R << 16`, Opus and PCM
  as `L << 16 | R`. One of them plays left and right swapped. Check with an L/R test signal per
  codec, then use one layout everywhere.
- Update libFLAC: the submodule is at 1.3.3+85 (July 2021, already has the 1.3.4 CVE fix), upstream
  is at 1.5.0. Upstream has oss-fuzz fixes for malformed frames. The most important one is a heap
  overflow when a frame's predictor order is larger than its blocksize (`2f209573`); others are
  `79e462fb` (partition order vs. blocksize), `1bec35e3` (residual overflow), `0e1535cc`
  (blocksize 65536) and `c65ef589` (invalid subframes). These only matter with a broken or hostile
  server, but mDNS discovery accepts any server on the LAN. Seeking, Ogg, 32-bit and MD5 fixes
  don't apply to us.
  <details>
  <summary>RAM trade-off (measured with 1.5.0 on an ESP32 without PSRAM)</summary>

  - About 2 KB more decoder heap (45.2 vs 43.1 KB).
  - `read_frame_()` keeps a 3.5 KB `FLAC__Frame` on the stack to insert silence for missing frames
    (new in 1.4.0, no compile switch to turn it off). The `http` task then peaks at 4.8 KB with
    FLAC instead of 2.5 KB, which overflows the 4 KB stack used when Opus is disabled (#255) and
    crashes on the first frames.
  - Options: raise the `http` stack to 6 KB (+2 KB RAM), patch the padding out of
    `stream_decoder.c`, or stay on 1.3.3 and backport only the small checks above. Patch libFLAC
    as a copy in the build directory from `components/flac/CMakeLists.txt`, not in the submodule.
  - The 1.5.0 build also needs `metadata_iterators.c` excluded (it calls `lstat()`, which newlib
    lacks; snapclient doesn't use it).

  </details>

## 3. Features

- Add missing codecs (Ogg).

## 4. Cleanup and documentation

- Wi-Fi TX power (#232) follow-ups: fold the boot-time TX power block, which appears twice in
  `wifi_start()`, into one helper; clamp the stored value at boot (only the web handler clamps to
  34–80); log errors from `esp_wifi_set_max_tx_power()`.
- Remove unused `tas5805m_settings_get_json()` and `tas5805m_settings_get_schema_json()`.
- Put Kconfig options in better places in the tree.
- Replace the ADF components copied into the tree with files pulled in via `CMakeLists.txt`.
- Clean up code (unused variables etc.).
- Fill in missing component descriptions in the README; improve documentation in general.
- Template `sdkconfig` files are tedious to maintain (#182).
