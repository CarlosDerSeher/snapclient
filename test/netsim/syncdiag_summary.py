#!/usr/bin/env python3
"""Sum the SYNCDIAG lines of a serial log per phase ("MARK <phase>" lines from run_test.sh).

usage: syncdiag_summary.py LOG [LOG ...]
Needs a build with CONFIG_SNAPCLIENT_SYNC_DIAG (components/diagnostics/README.md). Per phase:
- resyncs: init / HARD 1 / HARD 2 / late wire chunks skipped, and the queue minimum (chunks);
- silence: ms from each "RESYNCING HARD 2" to the next "initial sync";
- rx waits >= 100 ms and how many ended with a retransmit ("after loss"), with the longest;
- rx stalls >= 200 ms: count, how many ended with a retransmit ("in one go"), and their lengths.
Lines before the first MARK count as phase "start".
"""
import re
import sys


def summarize(path):
    phases = {}
    phase = 'start'
    hard2_at = None

    def get(p):
        return phases.setdefault(p, dict(init=0, h1=0, h2=0, late=0, qmin=None, silence=[],
                                          waits=0, wait_max=0, holes=0, hole_max=0,
                                          stalls=[], stalls_loss=0))
    for line in open(path, errors='replace'):
        line = re.sub(r'\x1b\[[0-9;]*m', '', line)
        m = re.search(r'MARK (.*)', line)
        if m:
            phase = m.group(1).strip()
            get(phase)
            continue
        d = get(phase)
        m = re.search(r'\((\d+)\) PLAYER: RESYNCING HARD 2', line)
        if m:
            hard2_at = int(m.group(1))
        m = re.search(r'\((\d+)\) PLAYER: initial sync', line)
        if m and hard2_at is not None:
            d['silence'].append(int(m.group(1)) - hard2_at)
            hard2_at = None
        m = re.search(r'resync init (\d+) hard1 (\d+) hard2 (\d+)(?: late (\d+))?', line)
        if m:
            d['init'] += int(m[1]); d['h1'] += int(m[2]); d['h2'] += int(m[3])
            d['late'] += int(m[4] or 0)
        m = re.search(r'queue min (-?\d+)', line)
        if m and int(m[1]) >= 0:
            d['qmin'] = int(m[1]) if d['qmin'] is None else min(d['qmin'], int(m[1]))
        m = re.search(r'rx waits (\d+) \(max (\d+) ms\) after loss (\d+) \(max (\d+) ms\)', line)
        if m:
            d['waits'] += int(m[1]); d['wait_max'] = max(d['wait_max'], int(m[2]))
            d['holes'] += int(m[3]); d['hole_max'] = max(d['hole_max'], int(m[4]))
        m = re.search(r'rx stall (\d+) ms', line)
        if m:
            d['stalls'].append(int(m[1]))
            d['stalls_loss'] += 'in one go' in line
    return phases


for path in sys.argv[1:]:
    print(path)
    for p, d in summarize(path).items():
        print(f"  {p:12s} init {d['init']} hard1 {d['h1']} hard2 {d['h2']} late {d['late']} "
              f"queue min {d['qmin']} silence ms {d['silence']}")
        print(f"  {'':12s} rx waits {d['waits']} (max {d['wait_max']} ms) after loss {d['holes']} "
              f"(max {d['hole_max']} ms); rx stalls {len(d['stalls'])} "
              f"({d['stalls_loss']} after loss) {d['stalls']}")
