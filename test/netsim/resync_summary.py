#!/usr/bin/env python3
"""Summarize a snapclient serial log: initial syncs, hard resyncs, pool/queue lines, errors.

Runs of consecutive "RESYNCING HARD 1" lines are folded into one line with a count.
The firmware prints at most one RESYNCING HARD line per second and adds "(+N suppressed)";
the N skipped resyncs (HARD 1 and 2 together) are counted with the kind of the printed line.
usage: resync_summary.py LOG [--counts]
"""
import re
import sys


def suppressed(msg):
    m = re.search(r'\(\+(\d+) suppressed\)', msg)
    return int(m.group(1)) if m else 0


def events(path):
    for line in open(path, errors='replace'):
        line = re.sub(r'\x1b\[[0-9;]*m', '', line)
        m = re.match(r'[IWE] \((\d+)\) (\w+): (.*)', line)
        if not m:
            continue
        t, msg = int(m.group(1)), m.group(3)
        if 'RESYNCING HARD 1' in msg:
            yield t, 'H1', 1 + suppressed(msg)
        elif 'RESYNCING HARD 2' in msg:
            age = re.search(r'age (-?\d+)us', msg)
            yield t, 'H2 %sus' % (age.group(1) if age else '?'), 1 + suppressed(msg)
        elif 'initial sync age' in msg:
            yield t, 'SYNC', 1
        elif 'pcm pool' in msg and 'dropped' in msg:
            yield t, 'POOL ' + msg.split(': ', 1)[-1], 1
        elif re.search(r'Guru|abort\(|Backtrace|reconnect', msg, re.I):
            yield t, 'ERR ' + msg[:100], 1


def main():
    ev = list(events(sys.argv[1]))
    if '--counts' in sys.argv:
        h1 = sum(n for _, k, n in ev if k == 'H1')
        h2 = sum(n for _, k, n in ev if k.startswith('H2'))
        syncs = sum(1 for _, k, _ in ev if k == 'SYNC')
        errs = sum(1 for _, k, _ in ev if k.startswith('ERR'))
        print('syncs %d  hard1 %d  hard2 %d  errors %d' % (syncs, h1, h2, errs))
        return
    i = 0
    while i < len(ev):
        t, k, n = ev[i]
        if k == 'H1':
            j = i
            while j + 1 < len(ev) and ev[j + 1][1] == 'H1':
                j += 1
            count = sum(e[2] for e in ev[i:j + 1])
            print('%8.2fs  H1 x%d until %.2fs' % (t / 1000, count, ev[j][0] / 1000))
            i = j + 1
            continue
        print('%8.2fs  %s%s' % (t / 1000, k, ' x%d' % n if n > 1 else ''))
        i += 1


if __name__ == '__main__':
    main()
