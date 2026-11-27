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

### 1.1 Task stacks
- With Ethernet: `playback_monitor_task` (4 KB, permanent) and `static_ip_task` (4 KB, created at
  runtime) → event handler or `esp_timer`. `eth_on_playback_stopped()` can block for up to 2.6 s, so
  it cannot simply run as an `esp_timer` callback.

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
- A PlatformIO env for it, and the README "Boards without PSRAM" table measured with it.

### 1.8 UDP logger on by default?
The stack cost is fixed (ec187dd): lines are formatted into a fixed buffer pool, so a logging task
needs 50–140 B extra stack instead of 520–640 B, and lines are dropped when the pool is full. It is
still off in `sdkconfig.defaults` (its Kconfig defaults to `y`) because the pool costs
`QUEUE_DEPTH` × `MAX_LINE` = 32 × 256 B ≈ 8 KB plus two tasks.

- Lower the defaults (e.g. 16 × 192 B ≈ 3 KB) in our `sdkconfig.defaults`, then decide whether to
  turn it back on, build-checked against all envs.

## 2. Robustness and bugs

- Ethernet boot delay: `esp_eth_start()` blocks `app_main` for 4 s in ~1 of 3 resets when no cable
  is plugged in (auto-negotiation timeout). Lower `autonego_timeout_ms` in
  `components/network_interface/eth_interface.c`, then check boot timing and that a plugged-in cable
  still gets link and IP.
- Hard resyncs: #191, #226 (fixes on experimental, waiting for tests; close when merged to develop).
- Stream change gives no audio (#111): when snapserver was started before the client, switching
  the client's stream doesn't deliver the new codec header, the server logs a socket error, and it
  takes several switches to get audio. Reproduce with `test/netsim/` (start `testserver.sh` first,
  then boot the board, then `client.py stream`), check whether the codec header arrives.
- Stalls under packet loss (#261), server side. With 2 % loss (netem) the server's congestion
  window shrinks to 3–4 segments, so a lost last segment of a chunk can't be fast-retransmitted and
  waits for Linux's retransmit timeout (min 200 ms); a lost retransmit doubles it to 0.7–1 s, which
  empties a 750 ms buffer (HARD 2). The client ACKs each read segment now, which removed the
  spurious retransmits; the rest only the server can fix. Measured on the server host (LyraT,
  5 min, loss both ways):
  - `ip route change <LAN subnet> dev <iface> rto_min 50ms`: stalls ≥ 200 ms ~30 → 0–2, no HARD 2.
  - `sysctl -w net.ipv4.tcp_thin_linear_timeouts=1`: short stalls stay, no doubling, no HARD 2.
  - Snapserver could set these per stream socket: `TCP_THIN_LINEAR_TIMEOUTS` (any kernel) and
    `TCP_RTO_MIN_US` (new kernels, check the version). Discuss in #261, then propose upstream
    or document the host settings in the README.
- Time sync while idle on Wi-Fi: after a playback, with Wi-Fi power save on while idle, time replies
  come back 10-230 ms late (median; MIN_MODEM ~10-25 ms, MAX_MODEM 17-230 ms), while pings show
  the board asleep either way. Before the first playback they take 2 ms despite power save (the
  station stays awake 50 ms after sending). Not light sleep (no change with a no-light-sleep lock
  per request), not Nagle (no change with `TCP_NODELAY`), not fixed by leaving power save per
  request. Maybe the AP's handling after a high-throughput download; needs a capture on the air.
  Meanwhile the time filter is reset at playback start only after idle Wi-Fi power save (~0.5 s
  refill); over Ethernet the idle estimate stays within 40 us and playback starts without it.
- Time filter start under packet loss (optional): the filter takes its very first sample as it is,
  and under loss a sample can be up to 690 ms off (the estimate was off for 0.4 s once; playback
  starts only after 29 samples, by then it was always within 0.1 ms). While filling, skip samples
  whose round trip is far above the lowest so far. `CONFIG_SNAPCLIENT_SYNC_DIAG` logs the first 48
  samples after the filter starts empty.
- Sync between boards. Measured with `test/sync/` (branch `sync-measure`, not merged yet): two
  boards' analog outputs into the PC's Line In, offset per 0.5 s window from cross-correlating a
  pink-noise stream. LyraT 4.3 vs ADAU1961 board, 200 s each:
  - A constant ~400 µs offset (ADAU1961 later) from the codecs' output latency, compensated per
    DAC with `CONFIG_DAC_GROUP_DELAY_FRAMES`; values missing, see "DAC group delays" below.
  - The control loop corrects only when the median error passes ±128 µs (`SHORT_OFFSET`),
    then inserts or drops one sample per chunk, which shows up as ~230 µs steps between boards.
  - The time filter's drift estimate stays 0, while the board crystals drift 3.7–5.8 ppm
    against the server; the offset is only followed, never predicted.
  - The LyraT's time round trip is ~5 ms (minimum 1.7 ms) even with arrival stamps, so its
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
- Sync drift fix (play time from the I2S DMA callbacks): tested at 44.1 kHz (11 h, no drift).
  Check 48 kHz too.
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
- Smoother sample insertion: to correct the sync, the player drops the last frame of a chunk or
  plays it twice, a one-frame step (22.7 µs at 44.1 kHz) at one point in the waveform. Rare in
  steady state (the I2S clock is ~5.5 ppm slow: one correction every 4–5 s), more often after a
  start or resync. Measure first: a loud 5–10 kHz tone, error in the windows with a correction
  vs. the rest (the worst SINAD windows in the channel order test, 21–24 dB vs. ~40 dB, may be
  these). If it shows, resample the corrected chunk from N to N∓1 frames with linear
  interpolation instead (a 0.09 % pitch change for one chunk, no step; 32-bit access only, chunks
  may be in IRAM).
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
  34–80); log errors from `esp_wifi_set_max_tx_power()`; drop the double blank line after the
  `wifi_tx_power` block in `ui_http_server.c`.
- Release tags: the firmware version is `git describe` (logged at boot and in the diagnostics).
  Tag test builds on `experimental` (e.g. `v0.0.5-exp.1`) so testers' logs show a readable version.
- Remove unused `tas5805m_settings_get_json()` and `tas5805m_settings_get_schema_json()`.
- Put Kconfig options in better places in the tree.
- Replace the ADF components copied into the tree with files pulled in via `CMakeLists.txt`.
- Clean up code (unused variables etc.).
- Fill in missing component descriptions in the README; improve documentation in general.
