# maxbuf: largest server buffer a board can play

Without PSRAM, the server buffer (`buffer` in snapserver.conf) decides how much RAM the client
needs: the PCM pool holds the whole buffer as decoded audio (about 176 KB per second for
44.1 kHz, 16 bit, stereo). `maxbuf.py` finds the largest buffer that still plays cleanly on a
given board and firmware. Use it after RAM changes and to keep the table in the main README
("Boards without PSRAM") up to date.

## How it works

For each buffer, smallest first, `maxbuf.py`:

1. restarts the test server (`test/netsim/testserver.sh`) with that buffer,
2. waits for the board to reconnect and switches it to the stream (default `flac44`, volume 1 %),
3. lets it play for `--dwell` seconds (default 45),
4. judges that part of the board's serial log.

A buffer is **ok** when the PCM pool was built, playback synced and there were no hard resyncs,
allocation errors, crashes or resets. Otherwise it prints the reason, for example "pool doesn't
fit" when the pool's DRAM reserve would be used up.

## Setup

Needs Python 3 with `pyserial`, `snapserver` and `ffmpeg` on this PC, and the board on the same
network. Run from the repository root.

1. Build the firmware with `CONFIG_SNAPCLIENT_HEAP_DIAG=y` (for the free-heap minimum) and flash
   it. Use the configuration you want to measure, e.g. `sdkconfig.defaults` for a plain ESP32.
2. Point the board at this PC, port 1704, so it uses the test server (once; survives flashing):
   ```sh
   test/netsim/client.py point BOARD PC_IP 1704
   ```
   To measure through the bad-Wi-Fi proxy instead, start `netsim_proxy.py` and use port 1706
   (see `test/netsim/README.md`).
3. Stop any other snapserver on this PC; the test server needs ports 1704, 1705 and 1780.
4. Run it:
   ```sh
   test/maxbuf/maxbuf.py --serial /dev/ttyUSB0 800 850 900 950 1000
   ```
   Opening the serial port resets the board. The log goes to `maxbuf-DATE.log` (or `--log FILE`).
   If another tool already logs the serial port, pass that file with `--log` and leave out
   `--serial`.
5. Afterwards: `test/netsim/client.py restore BOARD`.

Example output (plain ESP32, no DSP):

```
  800 ms  pool 30 x 4608 B          heap min  42.4 KB  ok
  850 ms  pool 32 x 4608 B          heap min  32.7 KB  ok
  875 ms  pool 33 x 4608 B          heap min  27.8 KB  ok
  900 ms  pool 34 x 4608 B          heap min  20.8 KB  ok
  925 ms  pool 35 x 4608 B          heap min  17.5 KB  ok
largest buffer that played cleanly: 925 ms
```

A buffer whose pool doesn't fit shows how many chunks did:

```
  875 ms  pool 33 x 4608 B (32 fit) heap min  20.8 KB  FAIL: pool doesn't fit, never synced
```

Results at the limit vary by a chunk between runs (how much the network holds while the pool is
built), so repeat the measurement before trusting a 25 ms difference.

## Notes

- The heap minimum is the lowest free heap since the board booted, so it carries over to the
  next buffer. Buffers run smallest first, so each value is still the minimum up to that buffer.
- One quiet run says nothing about bad Wi-Fi. The margin left at the maximum (heap min) is what
  absorbs Wi-Fi buffers and TCP backlog after an outage; check a buffer you want to recommend
  with the netsim scenario too.
- The streams are test tones. FLAC compresses them better than music, which doesn't matter for the
  pool (it holds decoded PCM) but makes the TCP backlog smaller than with real audio.
- `--stream flac48`, `opus` or `pcm` measure the other codecs; the chunk duration and so the pool
  size per buffer differ.
