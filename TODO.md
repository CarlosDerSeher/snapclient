# TODO

Open work on this project, roughly in priority order within each section. Pick something, open an
issue or a draft PR to say you are on it, and target `experimental`, which collects the recent
changes until they are merged into `develop` together.

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

### 1.1 Ethernet task stacks
`playback_monitor_task` (3 KB, permanent) and `static_ip_task` (3 KB while configuring) → event
handler or `esp_timer`. `eth_on_playback_stopped()` can block for up to 2.6 s, so it can't simply
run as an `esp_timer` callback.

### 1.2 Minimum-RAM sdkconfig
A defaults file for a plain ESP32 without PSRAM that collects everything that saves RAM: DSP off,
Opus off (`CONFIG_SNAPCLIENT_DISABLE_OPUS`, #255), UDP logger off. Add a PlatformIO env for it and
measure the README "Boards without PSRAM" table with it.

### 1.3 UDP logger on by default?
Its stack cost is fixed (ec187dd: lines go into a fixed buffer pool). It stays off in
`sdkconfig.defaults` because the pool costs 32 × 256 B ≈ 8 KB plus two tasks. Lower the pool (e.g.
16 × 192 B ≈ 3 KB), then decide whether to turn it back on; build-check all envs.

### 1.4 TAS5805M without PSRAM
All TAS5805M boards have PSRAM today. The driver needs ~1 KB static RAM plus a 3 KB fault task; the
blocker is the web UI, which builds large cJSON trees (and 1–64 KB buffers) for the DAC/EQ settings
and schemas. Needs a TAS5805M board to test.

- Measure the JSON size of `/api/dac/settings`, `/api/dac/schema`, `/api/eq/settings` and
  `/api/eq/schema`.
- Serve the schema (static per build) as a file from flash; generate only the current values and
  stream them like the DSP JSON (#252).
- POST: one parameter per request, or a small parser instead of `cJSON_Parse()` on the whole body.
- Fault task (polling) → periodic job without its own stack.

## 2. Robustness and bugs

### 2.1 Network

- Wi-Fi drops once right after the first got-IP on luar123's boards (#261; an S3 and an ESP32,
  Wi-Fi only, IDF 5.5.6): 0.2–1.3 s after got-IP the station disconnects, lwIP aborts the snapcast
  connection (`netconn err -13`) and the reconnect takes 2.5–5 s. Not caused by our code, and not
  seen on the LyraT here. Every disconnect is logged with its reason now (3b8c89c); check the
  reason in luar123's next log.
- Wi-Fi roaming: the station picks the strongest AP of the network only when it connects. After
  the LyraT's AP was rebooted it moved to another AP at -67 dBm and stayed there after the first
  one (-46 dBm) was back. Options: 802.11k/v (`rm_enabled`, `btm_enabled`) so APs can steer the
  station, or rescan when the RSSI stays low (`esp_wifi_set_rssi_threshold()`,
  `WIFI_EVENT_STA_BSS_RSSI_LOW`) and move to a stronger AP while idle, not during playback.
- Stalls under packet loss (#261), server side. Under loss the server's congestion window shrinks
  to 3–4 segments, so a lost last segment of a chunk waits for the retransmit timeout (min 200 ms,
  doubled to 0.7–1 s if the retransmit is lost too), which empties a 750 ms buffer. Measured on the
  server host (LyraT, netem 2 % loss): `rto_min 50ms` on the LAN route cut stalls ≥ 200 ms from ~30
  to 0–2 per 5 min; `net.ipv4.tcp_thin_linear_timeouts=1` stopped the doubling. Snapserver could
  set `TCP_THIN_LINEAR_TIMEOUTS` / `TCP_RTO_MIN_US` per stream socket. Waiting for luar123's
  answer in #261, then propose upstream or document the host settings in the README.
- Stream change gives no audio (#111): a client that restarts without closing its TCP connection
  leaves a stale session on snapserver, and `Group.SetStream` sends the codec header to the stale
  one. The client now resets its connection before every `esp_restart()` (0c6aa09); reset button,
  crashes and power loss need the server fix, reported as
  [snapcast/snapcast#1575](https://github.com/snapcast/snapcast/issues/1575).

### 2.2 Sync and playback

- Hard resyncs: #191, #226 (fixes on experimental, waiting for tests; close when merged to develop).
- Switching a playing board from `flac44` to the test server's `idle` stream (a 48 kHz pipe without
  data) gives one HARD 2 at about -51 ms right after the new start, with and without 92f10ee. Find
  out whether it is the format change or the empty stream.
- Sync between boards, measured with `test/sync/` (LyraT 4.3 vs ADAU1961 board, 200 s each):
  - ~400 µs constant offset from the codecs' output latency; see DAC group delays below.
  - The control loop corrects only past ±128 µs median error (`SHORT_OFFSET`), one sample per
    chunk, which shows up as ~230 µs steps between boards.
  - The time filter's drift estimate stays 0 while the crystals drift 3.7–5.8 ppm against the
    server: the offset is only followed, never predicted.
  - The LyraT's time round trip is ~5 ms (minimum 1.7 ms) even with arrival stamps, so the delay
    is before the board (Wi-Fi, TCP or server). Needs a packet capture.
- DAC group delays: `CONFIG_DAC_GROUP_DELAY_FRAMES` is 0 for every DAC except the ADAU1961 (25
  frames = 567 µs at 44.1 kHz). Take the others from the datasheet's interpolation filter table or
  measure them with `test/sync/` against the ADAU1961 board (same firmware and DSP settings; swap
  the Line In plugs for a second run to cancel the sound card's skew), then add a `default` per DAC
  in `components/audio_board/Kconfig.projbuild`:
  - ES8388 (LyraT, AI-Thinker): not in the datasheet; first measurement ~336 µs later than the
    ADAU1961, roughly 15 frames (one plug orientation only).
  - PCM5102A (PCM5100A/5101A): 20/fS normal filter, 3.5/fS low latency (the filter section says
    22/fS). PCM5121/5122: 20/fS. TAS57xx: probably the same, not confirmed.
  - TAS5805M: depends on the DSP configuration (0–127 samples for the FIR); measure with our
    default EQ.
  - MA120 / MA120X0, MAX98357, PT8211, and the other ESP-ADF boards (LyraTD-MSC, LyraT-Mini, Korvo
    DU1906, Kaluga): no datasheet value. PT8211 has no oversampling filter, so probably ~0.
  - ADAU1961 at 88.2/96 kHz: the driver runs the chip with 96 kHz as its base rate (R17 CONVSR at
    default); the datasheet's 11/fS is for a 48 kHz base. Check that this is in spec and measure.
- Idle clock with PM: while idle on Wi-Fi (no player PM lock: min 40 MHz plus light sleep),
  esp_timer runs ~1500–2200 ppm off the server with the RC as RTC clock and still ~1150 ppm with
  the ADAU1961 board's 32 kHz crystal; min_freq 80 MHz is worse (~3200–3500 ppm), tickless off
  much worse (~1.8 %). So it is PM/esp_timer, not the oscillator. The reset at playback start
  (cc7fa7c) handles it (0 HARD 2 vs. 4–11 without) and stays needed: IDF 5.5.5 drifts the same
  (LyraT, ~1500–2000 ppm; the 5.5.1 idle reply delay it fixed was the smaller part). For 180 s
  after boot Improv's PM lock blocks light sleep, so the clock stays accurate there.
  `RTC_CLK_SRC_INT_8MD256` boot-looped the LyraT on IDF 5.5.1 (RTC watchdog reset ~6 s after
  Wi-Fi connect); IDF 5.5.2 fixed a wakeup failure with it (`752e4e89`), retest on 5.5.5.
- Time sync on Wi-Fi: correct each request's c2s by its own driver wait,
  c2s − (TX done − sent), from `esp_wifi_set_tx_done_cb()` (private API; the callback has to
  parse every TX frame, and `data_len` is shorter than the frame). During playback the wait is
  1.2–1.8 ms median, p90 ~5 ms, so this could remove part of the round-trip noise noted below.
  Measure the time filter's noise during playback with and without it.
- Sync drift fix (play time from the I2S DMA callbacks): no drift in 11 h at 44.1 kHz. Check 48 kHz.
- Smoother sample insertion: a sync correction drops or repeats one frame (22.7 µs at 44.1 kHz), a
  step in the waveform; in steady state one every 4–5 s, more after a start or resync. Measure
  first (loud 5–10 kHz tone, error in windows with a correction vs. the rest). If it shows,
  resample the corrected chunk from N to N∓1 frames with linear interpolation (32-bit access
  only, chunks may be in IRAM).

### 2.3 Boards and codecs

- ADAU1961 with 24-bit streams: for more than 16 bits the player sets MCLK to 384 × fs, but the
  driver always sets R0 INFREQ to 256 × fs, and INFREQ has no 384 × fs setting. Use MCLK 768 × fs
  with INFREQ = 10, or the PLL. Test with a 24-bit stream.
- LyraT 4.3 with ESP32 rev v1.0: heap corruption at boot in ~1 of 5 boots shortly after got-IP
  (`udp_sendto()` in the UDP logger, or `heap_caps_free()` in the mDNS lookup), and pings answered
  only after 1–9 s for minutes after most restarts. The rev v3.0 LyraT with the same image is fine.
  Possibly a faulty board: try a second rev v1 board before spending more time on it.

### 2.4 Dependencies

- Update libFLAC: the submodule is at 1.3.3+85 (has the 1.3.4 CVE fix), upstream at 1.5.0 with
  oss-fuzz fixes for malformed frames; most important a heap overflow when a frame's predictor
  order is larger than its blocksize (`2f209573`), also `79e462fb`, `1bec35e3`, `0e1535cc`,
  `c65ef589`. Matters only with a broken or hostile server, but mDNS discovery accepts any server
  on the LAN.
  <details>
  <summary>RAM trade-off (measured with 1.5.0 on an ESP32 without PSRAM)</summary>

  - About 2 KB more decoder heap (45.2 vs 43.1 KB).
  - `read_frame_()` keeps a 3.5 KB `FLAC__Frame` on the stack to insert silence for missing frames
    (new in 1.4.0, no switch to turn it off). The `http` task then peaks at 4.8 KB with FLAC
    instead of 2.5 KB, which overflows the 4 KB stack used without Opus (#255).
  - Options: raise the `http` stack to 6 KB (+2 KB RAM), patch the padding out of
    `stream_decoder.c`, or stay on 1.3.3 and backport only the small checks. Patch a copy in the
    build directory from `components/flac/CMakeLists.txt`, not the submodule.
  - 1.5.0 also needs `metadata_iterators.c` excluded (it calls `lstat()`, which newlib lacks).

  </details>

## 3. Features

- Add missing codecs (Ogg).

## 4. Cleanup and documentation

- Wi-Fi TX power (#232) follow-ups: the boot-time TX power block appears twice in `wifi_start()`;
  fold it into one helper, clamp the stored value at boot (only the web handler clamps to 34–80)
  and log errors from `esp_wifi_set_max_tx_power()`.
- Compiler warnings in our code: unused `audioCodecCanSleep`, `uxHighWaterMark` and `MIN` in
  `player.c`, `mac_address` in `wifi_interface.c`, `waitStart` in `connection_handler.c`; the
  deprecated `smi_mdc_gpio_num` / `smi_mdio_gpio_num` in `eth_interface.c` (use `smi_gpio`).
- Remove unused `tas5805m_settings_get_json()` and `tas5805m_settings_get_schema_json()`.
- Release tags: tag test builds on `experimental` (e.g. `v0.0.5-exp.1`) so the `git describe`
  version in testers' logs is readable.
- Put Kconfig options in better places in the tree.
- Replace the ADF components copied into the tree with files pulled in via `CMakeLists.txt`.
- Fill in missing component descriptions in the README; improve documentation in general.
