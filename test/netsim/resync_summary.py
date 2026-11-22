#!/usr/bin/env python3
"""Summarize a snapclient serial log: initial syncs, hard resyncs, pool/queue lines, errors.

Runs of consecutive "RESYNCING HARD 1" lines are folded into one line with a count.
usage: resync_summary.py LOG [--counts]
"""
import re
import sys


def events(path):
    for line in open(path, errors='replace'):
        line = re.sub(r'\x1b\[[0-9;]*m', '', line)
        m = re.match(r'[IWE] \((\d+)\) (\w+): (.*)', line)
        if not m:
            continue
        t, msg = int(m.group(1)), m.group(3)
        if 'RESYNCING HARD 1' in msg:
            yield t, 'H1'
        elif 'RESYNCING HARD 2' in msg:
            age = re.search(r'age (-?\d+)us', msg)
            yield t, 'H2 %sus' % (age.group(1) if age else '?')
        elif 'initial sync age' in msg:
            yield t, 'SYNC'
        elif 'pcm pool' in msg and 'dropped' in msg:
            yield t, 'POOL ' + msg.split(': ', 1)[-1]
        elif re.search(r'Guru|abort\(|Backtrace|reconnect', msg, re.I):
            yield t, 'ERR ' + msg[:100]


def main():
    ev = list(events(sys.argv[1]))
    if '--counts' in sys.argv:
        h1 = sum(1 for _, k in ev if k == 'H1')
        h2 = sum(1 for _, k in ev if k.startswith('H2'))
        syncs = sum(1 for _, k in ev if k == 'SYNC')
        errs = sum(1 for _, k in ev if k.startswith('ERR'))
        print('syncs %d  hard1 %d  hard2 %d  errors %d' % (syncs, h1, h2, errs))
        return
    i = 0
    while i < len(ev):
        t, k = ev[i]
        if k == 'H1':
            j = i
            while j + 1 < len(ev) and ev[j + 1][1] == 'H1':
                j += 1
            print('%8.2fs  H1 x%d until %.2fs' % (t / 1000, j - i + 1, ev[j][0] / 1000))
            i = j + 1
            continue
        print('%8.2fs  %s' % (t / 1000, k))
        i += 1


if __name__ == '__main__':
    main()
