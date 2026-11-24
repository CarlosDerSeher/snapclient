# ethstress: stack peaks and Ethernet handover under load

`ethstress.py` runs one Ethernet board through everything that loads the Ethernet code and the
tasks around it, reads the serial log, and prints the lowest free stack per task. Use it before
and after changing task stack sizes or the Ethernet / Wi-Fi handover.

What it does, in order:

1. Resets the board, waits for playback.
2. Plays each test stream (`flac44`, `pcm`, `opus`, `noise44`, `flac48`) for 20 s. The whole time
   the web UI is queried every 0.15 s (main page, status, settings, DSP) and the volume changes
   on the server every 2 s.
3. Cable out and in during Opus and PCM playback (needs the [link test hook](#firmware)): Wi-Fi
   fallback, then Ethernet comes back, the takeover is deferred until playback stops (the `idle`
   stream), and the client reconnects over Ethernet.
4. Static IP (the board's own address, the PC's gateway): restart, playback, cable out and in
   again. Then back to the Ethernet settings the board had and restart.
5. With `--ota`: uploads a firmware image through the web UI (the heaviest receive load) and waits
   for the reboot.

At the end it restores the board's stream, volume and Ethernet settings and prints a summary.
The exit code is 1 if the log shows a crash (stack overflow, Guru Meditation, abort).

## Firmware

Build the board's usual config plus:

```
CONFIG_SNAPCLIENT_HEAP_DIAG=y
CONFIG_SNAPCLIENT_HEAP_DIAG_INTERVAL_S=5
CONFIG_SNAPCLIENT_WEB_UI_STATUS=y
CONFIG_SNAPCLIENT_ETH_LINK_TEST=y
```

- `HEAP_DIAG` writes the stack high-water marks the summary is built from. A 5 s interval
  catches the values right before the OTA reboot. The static IP task and the ping task only live
  while the static IP is set up; they log their own value when they end.
- `ETH_LINK_TEST` (internal Ethernet only, *Snapclient Ethernet Configuration*) lets the script
  pull the "cable": UDP port 9997, `d` powers the PHY down, `u` up again. The switch sees a real
  link loss. **Testing only:** anyone on the LAN can switch the link. Without it the cable steps
  are skipped. SPI Ethernet has no hook: powering down the W5500's PHY also stops its command
  engine, which a real unplug doesn't, so it would test the wrong thing. Pull the cable by hand
  there.
- Add `CONFIG_ESP32_UDP_LOGGER_ENABLED=y` to measure with the UDP logger (about 50-140 B more
  per task that logs). Without PSRAM its line buffers can leave too little RAM for the PCM pool at
  700 ms; `CONFIG_ESP32_UDP_LOGGER_QUEUE_DEPTH=8` helps.

## Setup

Needs Python 3 with pyserial, `snapserver` and `ffmpeg`. From the repository root:

1. Start the test server (it has the test streams and the silent `idle` stream):
   ```sh
   test/netsim/testserver.sh 700 &
   ```
2. Point the board at it directly, without the proxy (the setting survives flashing):
   ```sh
   python3 test/netsim/client.py point BOARD_IP PC_IP 1704
   ```
3. Run the test (about 6 min; the board must be on Ethernet and in DHCP or static mode):
   ```sh
   python3 test/ethstress/ethstress.py --serial /dev/ttyUSB1 --board 192.168.1.140 \
       --ota build/snapclient.bin
   ```
   Useful options: `--no-cable` (boards without the hook), `--no-static`, `--streams`,
   `--hold`, `--static-ip`, `--gateway`. `--help` lists them all.
4. Afterwards `python3 test/netsim/client.py restore BOARD_IP` points the board back at mDNS.

The script opens the serial port and resets the board through RTS. Don't flash while it runs.

## Output

Progress, then the summary (ADAU1961 board, LAN8720, no UDP logger, with `--ota`):

```
Lowest free stack per task (bytes, HEAPDIAG hwm):
  ipc0                456
  ipc1                496
  eth_ltest           640
  IDLE1               692
  IDLE0               696
  player              840
  ...
  emac_rx            1080
  eth_stati          1116
  eth_playb          1172
  ping               1320
  ...
  httpd              2220
  wifi               2540
  sys_evt            2720

boots 4 (expected 4), playback starts 19, pool didn't fit 0, web requests 1465 ok 11 failed
timeouts 0, crashes 0
serial log: ethstress-192.168.1.140.log
```

- **hwm** is the unused part of the task's stack at its fullest point. Below 512 B the summary
  marks it `<-- low`; in the 200s or less the size is too tight. Interrupts store their frame on
  the stack of the task they interrupt, so the peak varies by a few hundred bytes between runs:
  run it more than once before trimming a stack. The IDF tasks `ipc0`/`ipc1` have a fixed small
  stack and are never marked.
- **boots** counts the resets after the script's own reset: that one, the two restarts and the
  OTA reboot. More than expected means a crash; look in the serial log.
- **pool didn't fit**: the PCM pool for that stream's chunk size didn't fit into RAM, so that
  stream didn't play (common for `flac48` at 700 ms on boards without PSRAM). The load still
  runs.
- **web requests failed**: a few fail while the board restarts. Many more mean the web server
  is starved.
- **timeouts**: an expected log line didn't appear (playback didn't start, the takeover didn't
  happen). The serial log has a `########` line at each step to find where.
