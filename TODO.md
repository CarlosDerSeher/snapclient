# TODO

Open work on this project, roughly in priority order within each section. Pick something, open an
issue or a draft PR to say you are on it, and target `develop`.

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

Done: boot transients and IRAM → flash (#247). *In progress:* standalone OTA server replaced by web
OTA (#249), UI task removed and DSP JSON streamed (#252).

### 1.1 Task stacks
- Measure all stacks, then trim. The `http` task (15 KB) had 12.8 KB unused with FLAC; Opus
  (`VAR_ARRAYS`) must be measured before trimming.
- With Ethernet: `playback_monitor_task` (4 KB, permanent) and `static_ip_task` (4 KB, created at
  runtime) → event handler or `esp_timer`. `eth_on_playback_stopped()` can block for up to 2.6 s, so
  it cannot simply run as an `esp_timer` callback.

### 1.2 FLAC input without the wire-chunk buffer
Today every wire chunk is `malloc`ed (variable size, ~1.5–4 KB), copied into libFLAC's bitreader and
freed.

- `read_callback` reads straight from the connection (`read_data()`), bounded by a remaining-bytes
  counter for the current wire chunk. libFLAC's bitreader becomes the only input buffer.
- Network errors inside `read_callback` abort decoding and restart the connection; the decode loop
  in `handle_chunk_message()` must be bounded.
- Keep the `cachedBlocks` timestamp correction.
- Quick win: in `parse_wire_chunk_message()`, only allocate `inData` for Opus/FLAC; the PCM codec
  never uses it (~4.6 KB per chunk).
- Opus: one static buffer sized to the maximum packet instead of a `malloc` per chunk.

### 1.3 Fixed PCM chunk pool
*Draft PR #258 (stacked on #256).* Replace the per-chunk `malloc` and the unused fragmented-chunk
code with a fixed pool. Mainly a robustness change: no fragmentation, no "malloc failed, retry"
loops.

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
*Done, PR to come (stacked on 1.3).* Tested with simulated outages and real packet loss
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
relying on `sdkconfig.defaults`, which today still builds the UDP logger (its Kconfig defaults to
`y`, and `sdkconfig.defaults` doesn't turn it off).

- UDP logger off, DSP off, Opus optional (`CONFIG_SNAPCLIENT_DISABLE_OPUS`, #255).
- The Wi-Fi / lwIP settings from 1.4 once they are tested.
- A PlatformIO env for it, and the README "Boards without PSRAM" table measured with it.

## 2. Robustness and bugs

- *In progress (#253):* `player_task` returns on errors; a FreeRTOS task must never `return`
  (log, clean up, `vTaskDelete(NULL)`).
- Ethernet boot delay: `esp_eth_start()` blocks `app_main` for 4 s in ~1 of 3 resets when no cable
  is plugged in (auto-negotiation timeout). Lower `autonego_timeout_ms` in
  `components/network_interface/eth_interface.c`, then check boot timing and that a plugged-in cable
  still gets link and IP.
- Hard resyncs: #117, #191, #226.
- `DELETE /delete?param=snapserver_host` answers 500 when no host is stored:
  `settings_set_server_host(NULL)` (`components/settings_manager/settings_manager.c`) returns
  `ESP_ERR_NVS_NOT_FOUND` from `nvs_erase_key()`. All other clear functions treat a missing key as
  cleared; do the same here.
- Hard resyncs at startup under packet loss: playback starts with a server time offset that is off
  by up to ~150 ms, then hard-resyncs every 2.6 s (up to 4 times) while the estimate converges.
  Seen in every run with 2 % loss (`test/netsim/netem.sh`), never without loss. Likely cause: time
  replies share the TCP connection with the audio, so a retransmission delays them by 200 ms or more
  (offset error ≈ delay / 2). The time filter counts as full after 29 samples taken 10 ms apart
  during the startup burst, takes the first sample as the offset and the first two as the drift,
  and then samples only once per second. Confirm by logging each sample (round trip, offset, filter
  estimate) for the first seconds. Possible fixes:
  - count the filter as full only after a minimum time span (e.g. 2–3 s of fast sync), not after
    29 samples;
  - ignore samples whose round trip is far above the lowest seen so far (as NTP does);
  - start from the sample with the shortest round trip instead of the first one.
- Rate-limit the "RESYNCING HARD 1/2" warnings in `player_task` (#234, closed, credit W-Floyd): at
  most one line per second with one shared timestamp at file scope, query Wi-Fi/heap info only when
  a line is printed, and report how many were suppressed ("(+12 suppressed)"). Today every hard
  resync calls `esp_wifi_sta_get_ap_info()` and `heap_caps_*` in the high-priority player task, and
  a ~130-char line blocks ~11 ms at 115200 baud once the UART buffer is full. Separate PR from 1.3.
- Channel order differs between codecs: FLAC packs a stereo frame as `L | R << 16`, Opus and PCM
  as `L << 16 | R`. One of them plays left and right swapped. Check with an L/R test signal per
  codec, then use one layout everywhere.
- `player_task` computes the frame size in its I2S write loop as `ch + (bits >> 3)` instead of
  `ch * (bits >> 3)` (also in the disabled `#else` variant). Correct by chance only for 16-bit
  stereo; wrong sample counts and chunk timing for mono, 24 or 32 bit.
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
