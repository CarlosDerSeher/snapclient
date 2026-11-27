#!/usr/bin/env python3
"""Summarize ss_sample.py output: how the server's TCP connection to the board behaved.

usage: ss_summary.py SAMPLES [SAMPLES ...]
Uses the connection with the most samples (a reconnect makes a new one). Prints:
- ack delay: ms since the last ACK while data is unacknowledged (p50/p90/p99). High values mean
  the server waits for the client's ACK;
- srtt p50, retransmit timeout p50/max, highest backoff (each backoff doubles the timeout);
- cwnd mean and most common values, total retransmits, share of samples marked app_limited.
"""
import collections
import re
import sys


def num(s, key, default=0.0):
    m = re.search(r'\b' + key + r':([\d.]+)', s)
    return float(m[1]) if m else default


for path in sys.argv[1:]:
    rows = []
    for line in open(path):
        p = line.split(' ', 1)
        if len(p) < 2 or 'cwnd' not in p[1]:
            continue
        tok = p[1].split()
        rows.append((tok[3].rsplit(':', 1)[1], p[1]))   # peer port
    if not rows:
        print(f"{path}: no samples")
        continue
    peer = collections.Counter(r[0] for r in rows).most_common(1)[0][0]
    L = [s for port, s in rows if port == peer]
    q = lambda a, x: a[min(len(a) - 1, int(len(a) * x))] if a else 0
    ack = sorted(num(s, 'lastack') for s in L if num(s, 'unacked') > 0)
    rtt = sorted(num(s, 'rtt') for s in L)
    rto = sorted(num(s, 'rto') for s in L)
    cw = [int(num(s, 'cwnd')) for s in L]
    m = re.search(r'retrans:\d+/(\d+)', L[-1])
    print(f"{path}: {len(L)} samples, ack delay p50 {q(ack, .5):.0f} p90 {q(ack, .9):.0f} "
          f"p99 {q(ack, .99):.0f} ms | srtt p50 {q(rtt, .5):.1f} ms | rto p50 {q(rto, .5):.0f} "
          f"max {rto[-1]:.0f} ms, backoff max {max(int(num(s, 'backoff')) for s in L)} | "
          f"cwnd mean {sum(cw) / len(cw):.1f} {collections.Counter(cw).most_common(4)} | "
          f"retrans {m[1] if m else 0} | app_limited {sum('app_limited' in s for s in L) / len(L):.0%}")
