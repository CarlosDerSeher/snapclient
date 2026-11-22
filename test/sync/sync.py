#!/usr/bin/env python3
"""Measure how well two snapclient boards play in sync.

Board A's left output goes to the left Line In of this PC's sound card, board B's left output to
the right Line In. Both channels are sampled by the same ADC, so the time offset between the two
recorded channels is the offset between the two boards. The boards play the noise44 stream of
test/netsim/testserver.sh (pink noise, no silence, so FLAC compresses it like music).

  sync.py live [--seconds S] [--wav FILE]
                                     measure while recording, one line per window, summary
                                     at Ctrl-C
  sync.py run SECONDS [--wav FILE]   record from Line In, then analyze
  sync.py analyze FILE               analyze a stereo WAV recorded earlier
  sync.py selftest                   analyze a synthetic recording with known offsets

The recording is cut into windows (default 0.5 s, back to back, so every sample is used;
--step shorter than --window makes them overlap for more values per second). For each window the two channels are
cross-correlated (band-passed, so hum and DC don't pull the peak to 0) and the peak is located
to a fraction of a sample. Positive offset = board B plays later than board A.

Output: per-window lines with --csv, and a summary: median, spread, drift, jumps (offset changed
by more than --jump-us between two windows, e.g. a hard resync), and windows without a match
(one board silent or playing something else: dropout, stopped player, wrong stream).

Calibrate once with one board into both inputs (Y cable): the offset should be ~0. Pass a
non-zero result as --cal-us to later runs.

Needs numpy (python3 -m venv ~/.venv-sync && ~/.venv-sync/bin/pip install numpy).
"""

import argparse
import subprocess
import sys
import wave

import numpy as np


def read_wav(path):
    with wave.open(path, "rb") as w:
        ch, width, rate = w.getnchannels(), w.getsampwidth(), w.getframerate()
        raw = w.readframes(w.getnframes())
    if ch != 2 or width not in (2, 4):
        sys.exit(f"{path}: need 2 channels with 16 or 32 bit, got {ch} channels, {8 * width} bit")
    x = np.frombuffer(raw, dtype="<i2" if width == 2 else "<i4").reshape(-1, 2)
    x = x.astype(np.float64) / 2 ** (8 * width - 1)
    return rate, x[:, 0], x[:, 1]


def record(a):
    wav = a.wav or "sync.wav"
    cmd = ["arecord", "-D", a.device, "-f", "S16_LE", "-r", str(a.rate), "-c", "2",
           "-d", str(a.seconds), wav]
    print(" ".join(cmd), file=sys.stderr)
    subprocess.run(cmd, check=True)
    return wav


def level_db(x):
    return 20 * np.log10(max(np.sqrt(np.mean(x * x)), 1e-9))


class Meter:
    """Offset between the two channels of one window."""

    def __init__(self, rate, args):
        self.rate, self.args = rate, args
        self.n = int(args.window * rate)
        self.step = int((args.step or args.window) * rate)
        maxlag = int(args.max_ms * rate / 1000)
        self.nfft = 1 << int(np.ceil(np.log2(self.n + maxlag)))
        f = np.fft.rfftfreq(self.nfft, 1 / rate)
        self.band = (f >= args.lo) & (f <= min(args.hi, rate / 2 * 0.95))
        self.lim = maxlag * args.upsample

    def window(self, t, sa, sb):
        """(time_s, offset_us or None, coefficient, level A, level B, reason)"""
        args, nfft, up, lim = self.args, self.nfft, self.args.upsample, self.lim
        la, lb = level_db(sa), level_db(sb)
        if la < args.silence_db or lb < args.silence_db:
            return (t, None, 0.0, la, lb, "silent " + ("A" if la < args.silence_db else "B"))
        A = np.fft.rfft(sa, nfft) * self.band
        B = np.fft.rfft(sb, nfft) * self.band
        # c[k] = sum b[i + k] a[i]: peaks at k = d if B is A delayed by d samples
        c = np.fft.irfft(B * np.conj(A), nfft * up) * up
        c = np.concatenate((c[-lim:], c[:lim + 1]))
        i = int(np.argmax(np.abs(c)))
        energy = np.sqrt((2 * np.sum(np.abs(A) ** 2) / nfft) * (2 * np.sum(np.abs(B) ** 2) / nfft))
        coef = c[i] / energy
        frac = 0.0
        if 0 < i < len(c) - 1:
            y0, y1, y2 = np.abs(c[i - 1:i + 2])
            den = y0 - 2 * y1 + y2
            if den:
                frac = 0.5 * (y0 - y2) / den
        if abs(coef) < args.min_coef:
            return (t, None, coef, la, lb, "no match")
        lag = (i - lim + frac) / up
        return (t, lag / self.rate * 1e6 - args.cal_us, coef, la, lb, "")


def measure(rate, a, b, args):
    m = Meter(rate, args)
    return [m.window(s / rate, a[s:s + m.n], b[s:s + m.n])
            for s in range(0, len(a) - m.n + 1, m.step)]


def fmt_row(row):
    t, off, coef, la, lb, why = row
    val = f"{off:9.1f} us" if off is not None else f"{'-':>9s}   "
    return f"{t:8.2f} s {val}  corr {coef:5.2f}  A {la:6.1f} dB  B {lb:6.1f} dB  {why}"


def live(args):
    """Record until Ctrl-C (or --seconds), measure each window as it arrives."""
    rate = args.rate
    cmd = ["arecord", "-q", "-D", args.device, "-f", "S16_LE", "-r", str(rate), "-c", "2",
           "-t", "raw"]
    if args.seconds:
        cmd += ["-d", str(args.seconds)]
    print(" ".join(cmd), file=sys.stderr)
    m = Meter(rate, args)
    wav = None
    if args.wav:
        wav = wave.open(args.wav, "wb")
        wav.setnchannels(2), wav.setsampwidth(2), wav.setframerate(rate)
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    buf = np.zeros((0, 2))
    pos = 0          # sample index of buf[0]
    rows, prev, clipped = [], None, False
    try:
        while True:
            raw = proc.stdout.read(m.step * 4)
            if not raw:
                break
            raw = raw[:len(raw) // 4 * 4]
            if wav:
                wav.writeframes(raw)
            x = np.frombuffer(raw, dtype="<i2").reshape(-1, 2) / 32768.0
            if not clipped and np.abs(x).max() > 0.99:
                clipped = True
                print("CLIPPING, lower the board volume or Capture level", file=sys.stderr)
            buf = np.concatenate((buf, x))
            while len(buf) >= m.n:
                row = m.window(pos / rate, buf[:m.n, 0], buf[:m.n, 1])
                rows.append(row)
                mark = ""
                if row[1] is not None and prev is not None and abs(row[1] - prev) > args.jump_us:
                    mark = f"  JUMP {row[1] - prev:+.1f} us"
                if row[1] is not None:
                    prev = row[1]
                if not args.quiet or mark or row[1] is None:
                    print(fmt_row(row) + mark, flush=True)
                buf = buf[m.step:]
                pos += m.step
    except KeyboardInterrupt:
        pass
    finally:
        proc.terminate()
        proc.wait()
        if wav:
            wav.close()
    print()
    return report(rows, args)


def report(rows, args):
    if args.csv:
        with open(args.csv, "w") as out:
            out.write("time_s,offset_us,coef,level_a_db,level_b_db,note\n")
            for t, off, coef, la, lb, why in rows:
                out.write(f"{t:.3f},{'' if off is None else f'{off:.2f}'},{coef:.3f},"
                          f"{la:.1f},{lb:.1f},{why}\n")
        print(f"per-window values: {args.csv}")

    good = [(t, off, coef) for t, off, coef, _, _, _ in rows if off is not None]
    bad = [(t, why) for t, off, _, _, _, why in rows if off is None]
    print(f"windows: {len(rows)} x {args.window} s every {args.step or args.window} s, "
          f"matched {len(good)}, no match {len(bad)}")
    if not good:
        print("no window matched: check wiring, Line In selected, both boards on noise44")
        return 1
    t = np.array([g[0] for g in good])
    off = np.array([g[1] for g in good])
    coef = np.array([g[2] for g in good])
    if np.median(coef) < 0:
        print("note: the channels are inverted against each other (negative correlation)")
    print(f"offset B-A [us]: median {np.median(off):.1f}  mean {off.mean():.1f}  "
          f"std {off.std():.1f}  min {off.min():.1f}  max {off.max():.1f}  "
          f"p-p {off.max() - off.min():.1f}")
    p5, p95 = np.percentile(off, [5, 95])
    print(f"                 5..95 % {p5:.1f} .. {p95:.1f}  "
          f"(|offset| < 100 us in {np.mean(np.abs(off) < 100) * 100:.0f} % of windows)")
    if len(t) > 2 and t[-1] > t[0]:
        slope = np.polyfit(t, off, 1)[0] * 60
        print(f"drift {slope:+.1f} us/min, correlation median {np.median(np.abs(coef)):.2f}")

    jumps = [(t[i], off[i - 1], off[i]) for i in range(1, len(off))
             if abs(off[i] - off[i - 1]) > args.jump_us]
    print(f"jumps > {args.jump_us:.0f} us: {len(jumps)}")
    for tj, before, after in jumps[:args.list]:
        print(f"  {tj:8.1f} s  {before:9.1f} -> {after:9.1f} us")
    if len(jumps) > args.list:
        print(f"  ... {len(jumps) - args.list} more")

    # merge consecutive unmatched windows into spans
    spans = []
    step = args.step or args.window
    for tb, why in bad:
        if spans and spans[-1][2] == why and abs(spans[-1][1] - tb) < 1e-6:
            spans[-1][1] = tb + step
        else:
            spans.append([tb, tb + step, why])
    if spans:
        print("without match:")
        for s, e, why in spans[:args.list]:
            print(f"  {s:8.1f} .. {e:8.1f} s  {why}")
        if len(spans) > args.list:
            print(f"  ... {len(spans) - args.list} more")
    return 0


def pink(n, rng):
    X = np.fft.rfft(rng.standard_normal(n))
    f = np.arange(len(X))
    X[1:] /= np.sqrt(f[1:])
    X[0] = 0
    x = np.fft.irfft(X, n)
    return x / np.abs(x).max() * 0.3


def delay(x, d):
    """x delayed by d samples (fractional, circular)."""
    X = np.fft.rfft(x)
    return np.fft.irfft(X * np.exp(-2j * np.pi * np.fft.rfftfreq(len(x)) * d), len(x))


def selftest(args):
    rate, secs = 48000, 60
    rng = np.random.default_rng(1)
    n = rate * secs
    a = pink(n, rng)
    # B: 37.3 us late for 30 s, then a jump to -512.8 us; a 2 s dropout at 40 s
    b = np.concatenate((delay(a, 37.3e-6 * rate)[:n // 2], delay(a, -512.8e-6 * rate)[n // 2:]))
    b[40 * rate:42 * rate] = 0
    hum = 0.05 * np.sin(2 * np.pi * 50 * np.arange(n) / rate)
    a = a + hum + 0.003 * rng.standard_normal(n)
    b = 0.7 * b + hum + 0.003 * rng.standard_normal(n)
    rows = measure(rate, a, b, args)
    print("expected: 37.3 us for 0..30 s, then -512.8 us, one jump at 30 s, "
          "no match 40..42 s (silent B is hidden by hum: 'no match')")
    report(rows, args)
    first = [o for t, o, *_ in rows if o is not None and t < 29]
    second = [o for t, o, *_ in rows if o is not None and 31 < t < 39]
    err = max(np.abs(np.array(first) - 37.3).max(), np.abs(np.array(second) + 512.8).max())
    print(f"max error {err:.2f} us")
    return 0 if err < 2 else 1


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                formatter_class=argparse.RawDescriptionHelpFormatter,
                                epilog=__doc__.split("\n", 2)[2])
    p.add_argument("--window", type=float, default=0.5, help="window length in s (0.5)")
    p.add_argument("--step", type=float,
                   help="start a window every STEP s (default: --window, no overlap)")
    p.add_argument("--max-ms", type=float, default=20, help="largest offset searched (20 ms)")
    p.add_argument("--lo", type=float, default=200, help="band-pass low edge, Hz (200)")
    p.add_argument("--hi", type=float, default=15000, help="band-pass high edge, Hz (15000)")
    p.add_argument("--upsample", type=int, default=8, help="correlation interpolation (8)")
    p.add_argument("--min-coef", type=float, default=0.3,
                   help="correlation below this = no match (0.3)")
    p.add_argument("--silence-db", type=float, default=-60,
                   help="channel below this level (dBFS) = silent (-60)")
    p.add_argument("--jump-us", type=float, default=200, help="report jumps above this (200 us)")
    p.add_argument("--cal-us", type=float, default=0, help="subtract this (calibration run)")
    p.add_argument("--csv", help="write per-window values to this file")
    p.add_argument("--list", type=int, default=20, help="list at most this many jumps/gaps")
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("run", help="record from Line In, then analyze")
    s.add_argument("seconds", type=int)
    s.add_argument("--wav", help="keep the recording here (default sync.wav)")
    s.add_argument("--device", default="hw:1,0", help="ALSA capture device (hw:1,0)")
    s.add_argument("--rate", type=int, default=48000, help="sample rate (48000)")

    s = sub.add_parser("live", help="measure while recording, until Ctrl-C")
    s.add_argument("--seconds", type=int, help="stop after this many seconds")
    s.add_argument("--wav", help="also save the recording here")
    s.add_argument("--device", default="hw:1,0", help="ALSA capture device (hw:1,0)")
    s.add_argument("--rate", type=int, default=48000, help="sample rate (48000)")
    s.add_argument("--quiet", action="store_true",
                   help="print only jumps and windows without a match")

    s = sub.add_parser("analyze", help="analyze a stereo WAV")
    s.add_argument("file")

    sub.add_parser("selftest", help="synthetic recording with known offsets")

    args = p.parse_args()
    if args.cmd == "selftest":
        return selftest(args)
    if args.cmd == "live":
        return live(args)
    path = record(args) if args.cmd == "run" else args.file
    rate, a, b = read_wav(path)
    for name, x in (("A (left)", a), ("B (right)", b)):
        peak = np.abs(x).max()
        print(f"{name}: level {level_db(x):.1f} dBFS, peak {20 * np.log10(max(peak, 1e-9)):.1f} dBFS"
              + ("  CLIPPING, lower the board volume or Capture level" if peak > 0.99 else ""))
    if np.abs(a - b).max() < 1e-3:
        print("WARNING: both channels are identical: a mono input (Mic) is selected, or one board "
              "feeds both inputs. Select Line In (see README) unless this is the calibration run")
    return report(measure(rate, a, b, args), args)


if __name__ == "__main__":
    sys.exit(main())
