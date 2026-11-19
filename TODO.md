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
Replace the per-chunk `malloc` and the unused fragmented-chunk code with a fixed pool. Mainly a
robustness change: no fragmentation, no "malloc failed, retry" loops.

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
- Fold in #234 (closed, credit W-Floyd): rate-limit the "RESYNCING HARD" warnings in `player_task`
  to one per second, query Wi-Fi/heap info only when a line is printed, and report how many were
  suppressed. Related: #117, #191, #226.

Acceptance: no `malloc/realloc/free` from decode to playback after the pool is built; minimum free
heap and largest free block flat over 10 min of playback; sync unchanged across codec switches,
buffer changes on the server and reconnects.

</details>

### 1.4 Wi-Fi / lwIP tuning
Test under bad Wi-Fi before committing.

- Dynamic instead of static Wi-Fi TX buffers (−12.8 KB fixed).
- `ESP_WIFI_RX_BA_WIN` 16 → 6, static RX buffers 8 → 6, TCP window and send buffer 11680 → 5840.
- STA only: no SoftAP, no Enterprise; no IPv6 if not needed.
- Check `LWIP_MAX_UDP_PCBS`: DHCP, DNS and mDNS each need one.

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

## 2. Robustness and bugs

- *In progress (#253):* `player_task` returns on errors; a FreeRTOS task must never `return`
  (log, clean up, `vTaskDelete(NULL)`).
- Ethernet boot delay: `esp_eth_start()` blocks `app_main` for 4 s in ~1 of 3 resets when no cable
  is plugged in (auto-negotiation timeout). Lower `autonego_timeout_ms` in
  `components/network_interface/eth_interface.c`, then check boot timing and that a plugged-in cable
  still gets link and IP.
- Sample rate change without a reboot is missed: when the server switches from 48 kHz to
  44.1 kHz (e.g. FLAC 48 kHz → FLAC 44.1 kHz across a reconnect), the player keeps the 48 kHz chunk
  duration (24000 µs instead of 26122 µs), never logs `reinitializing i2s`, and hard-resyncs every
  ~2.6 s (`age -210 ms`). Suspects: `player_send_snapcast_setting()` drops the setting while the
  player task is stopped, and `handle_chunk_message()` only resends settings when the frame count
  changes.
- Hard resyncs: #117, #191, #226 (see 1.3).

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
