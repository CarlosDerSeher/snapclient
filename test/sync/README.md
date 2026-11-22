# sync: how well do two boards play in sync?

`sync.py` records the analog outputs of two boards with this PC's sound card and measures the time
offset between them, to about 1 µs.

```
board A left out  --> Line In left  (tip)  \
                                             PC sound card, one ADC
board B left out  --> Line In right (ring) /
```

Both channels are sampled by the same ADC on the same clock, so the offset between the recorded
channels is the offset between the boards. The sample rates of the boards (44.1 kHz) and the
sound card (48 kHz) don't need to match.

## Wiring

- A cable with two mono plugs into one stereo plug (or a stereo-to-2-mono cable used backwards):
  board A's left channel to the tip, board B's left channel to the ring, grounds together.
- Use the **Line In** jack (blue, rear). The Mic jacks are mono and put a bias voltage on the plug.
- Only line or headphone outputs (PCM5102A and similar). **Never** a speaker amplifier output
  (TAS5805M, MAX98357A, ...): their outputs are bridged and can damage the sound card input.
- Both boards powered from the same PC by USB is fine. Hum from a ground loop doesn't matter: the
  analysis ignores everything below 200 Hz.

## Sound card setup

The capture source must be Line In, with moderate gain (the commands are for the Realtek ALC897,
card 1; `arecord -l` lists the cards):

```sh
amixer -c 1 sset 'Input Source' Line
amixer -c 1 sset Capture 23          # 0 dB; 63 is +30 dB and clips
amixer -c 1 sset Capture cap
```

`alsamixer -c 1` (F4 for capture) does the same. With a Mic input selected, both channels record
the same mono signal and the result is a meaningless 0 µs; `sync.py` warns when the channels are
identical or clip.

## Test signal

`test/netsim/testserver.sh` has a stream `noise44`: FLAC 44.1 kHz, pink noise, different noise on
left and right. There is no silence, so FLAC compresses it like music (~70 % of PCM; silence or the
same signal on both channels would shrink to almost nothing and spare the client's Wi-Fi and
decoder). The noise never repeats, so the offset is unambiguous up to `--max-ms` (20 ms).

## Measuring

Needs `snapserver`, `ffmpeg`, `arecord` and numpy:

```sh
python3 -m venv ~/.venv-sync && ~/.venv-sync/bin/pip install numpy
~/.venv-sync/bin/python test/sync/sync.py selftest    # synthetic recording, known offsets
```

1. Start the test server and point both boards at it (port 1704 directly, no proxy):
   ```sh
   test/netsim/testserver.sh 750 &
   test/netsim/client.py point BOARD_A PC_IP 1704
   test/netsim/client.py point BOARD_B PC_IP 1704
   ```
2. Play `noise44` on both, at the same volume (with several boards connected, `--client` picks
   one; `client.py status` lists them):
   ```sh
   test/netsim/client.py stream noise44 --client BOARD_A_IP --volume 50
   test/netsim/client.py stream noise44 --client BOARD_B_IP --volume 50
   ```
3. Calibrate once: one board into both inputs (Y cable). The offset should be ~0 µs; pass
   anything else as `--cal-us` to later runs.
4. Record and analyze, e.g. 5 minutes:
   ```sh
   ~/.venv-sync/bin/python test/sync/sync.py --csv sync.csv run 300 --wav sync.wav
   ~/.venv-sync/bin/python test/sync/sync.py analyze sync.wav     # again, other options
   ```
5. Afterwards: `test/netsim/client.py restore BOARD` for both boards, stop the server.

Bad Wi-Fi can be added with `netsim_proxy.py` and `netem.sh` as described in
[`test/netsim/README.md`](../netsim/README.md).

## Output

Example (illustrative values):

```
offset B-A [us]: median 37.2  mean 37.3  std 4.1  min 28.0  max 46.9  p-p 18.9
                 5..95 % 31.0 .. 43.5  (|offset| < 100 us in 100 % of windows)
drift +0.3 us/min, correlation median 0.98
jumps > 200 us: 1
      30.0 s       37.3 ->    -512.8 us
without match:
      40.0 ..     42.0 s  no match
```

- **offset**: positive = board B plays later. Snapcast corrects in steps (sample insertion,
  soft and hard resync), so the offset wanders by some 10 µs around its median.
- **drift**: a slope means one board isn't corrected (clock or time-sync problem).
- **jumps**: the offset changed by more than `--jump-us` between two windows: hard resync, or a
  board that restarted its player.
- **without match**: one board silent (dropout, player stopped, muted) or playing something else.

Both boards need the same audio path: DSP settings (EQ filters delay the signal) and DAC type
(different DAC filters have different group delay) should match, or the offset includes that.
Each window is 0.5 s (`--window`); `--csv` writes every window for plotting.
