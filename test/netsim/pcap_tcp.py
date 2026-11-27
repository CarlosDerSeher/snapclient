#!/usr/bin/env python3
"""Analyse a packet capture of the audio stream: lost segments, server pauses and why they happened.

usage: pcap_tcp.py CAPTURE [--port 1704] [--min-segments 1000]
Capture on the server host, e.g.
  sudo tcpdump -i IFACE -s 128 -w cap.pcap host BOARD and tcp port 1704
(on the server, sent packets are seen after netem and received ones before netem's ingress drop).
Reads pcap with Ethernet or Linux cooked headers (-i any), IPv4. For each connection with at least
--min-segments data segments from the server:
- lost segments (a hole in the sequence numbers) and how long until the retransmit filled them;
- retransmits of segments the client already had (spurious: the server's timer fired while it
  waited for an ACK);
- pauses >= 150 ms between server data segments, sorted by what the server was waiting for:
  "delayed ACK" (the ACK for the last segment came late, >= 80 % of the pause),
  "tail loss" (no ACK for the last segment, then a retransmit: data or ACK lost, timer),
  "idle" (everything was acknowledged early; nothing to send);
- the client's ACK delay after the last segment before each pause.
"""
import argparse
import bisect
import collections
import struct


def read(path):
    d = open(path, 'rb').read()
    magic, = struct.unpack('<I', d[:4])
    ns = magic == 0xa1b23c4d
    linktype, = struct.unpack('<I', d[20:24])
    l2 = {1: 14, 113: 16, 276: 20}[linktype]
    off, out = 24, []
    while off + 16 <= len(d):
        ts, tu, cl, _ = struct.unpack('<IIII', d[off:off + 16])
        off += 16
        p = d[off:off + cl]
        off += cl
        proto = p[12:14] if linktype == 1 else p[14:16] if linktype == 113 else p[0:2]
        if proto != b'\x08\x00' or len(p) < l2 + 40:
            continue
        ip = p[l2:]
        if ip[9] != 6:
            continue
        ihl = (ip[0] & 15) * 4
        tot, = struct.unpack('>H', ip[2:4])
        tcp = ip[ihl:]
        sp, dp, seq, ack, thl = struct.unpack('>HHIIB', tcp[:13])
        out.append(dict(t=ts + tu / (1e9 if ns else 1e6), sp=sp, dp=dp, seq=seq, ack=ack,
                        len=tot - ihl - (thl >> 4) * 4))
    return out


def after(a, b):
    return 0 < ((a - b) & 0xffffffff) < 1 << 31


def analyse(data, acks, name):
    top, holes, spurious = None, [], 0
    for p in data:
        s, e = p['seq'], (p['seq'] + p['len']) & 0xffffffff
        if top is None or s == top:
            top = e
        elif after(s, top):
            holes.append(dict(start=top, seen=p['t'], fill=None))
            top = e
        else:
            h = next((h for h in holes if h['fill'] is None and h['start'] == s), None)
            if h:
                h['fill'] = p['t']
            else:
                spurious += 1
    ts = [p['t'] for p in data]
    rec = []
    for h in holes:
        if h['fill'] is not None:
            i = bisect.bisect_left(ts, h['seen']) - 1
            rec.append((h['fill'] - (ts[i] if i >= 0 else h['seen'])) * 1000)
    rec.sort()
    q = lambda a, x: a[min(len(a) - 1, int(len(a) * x))] if a else 0
    print(f"== {name}: {len(data)} data segments, {len(holes)} lost, recovered in p50 "
          f"{q(rec, .5):.0f} p90 {q(rec, .9):.0f} max {rec[-1] if rec else 0:.0f} ms; "
          f"spurious retransmits {spurious}")

    at = [a['t'] for a in acks]
    kinds, delays = collections.Counter(), []
    for i in range(1, len(data)):
        gap = (data[i]['t'] - data[i - 1]['t']) * 1000
        if gap < 150:
            continue
        last = data[i - 1]
        end = (last['seq'] + last['len']) & 0xffffffff
        cover = None
        for a in acks[bisect.bisect_right(at, last['t']):]:
            if a['t'] > data[i]['t'] + 0.001:
                break
            if not after(end, a['ack']):
                cover = (a['t'] - last['t']) * 1000
                break
        if cover is None:
            kinds['tail loss' if data[i]['seq'] != end else 'no ACK, then new data'] += 1
        else:
            delays.append(cover)
            kinds['delayed ACK' if cover >= 0.8 * gap - 1 else 'idle'] += 1
    delays.sort()
    print(f"   pauses >= 150 ms: {sum(kinds.values())} {dict(kinds)}; client ACK delay before "
          f"them p50 {q(delays, .5):.0f} p90 {q(delays, .9):.0f} ms")


ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('capture')
ap.add_argument('--port', type=int, default=1704, help="server stream port (default 1704)")
ap.add_argument('--min-segments', type=int, default=1000)
a = ap.parse_args()
P = read(a.capture)
conns = collections.Counter(p['dp'] for p in P if p['sp'] == a.port and p['len'] > 0)
for cport, n in sorted(conns.items()):
    if n < a.min_segments:
        continue
    data = [p for p in P if p['sp'] == a.port and p['dp'] == cport and p['len'] > 0]
    acks = [p for p in P if p['dp'] == a.port and p['sp'] == cport]
    t0 = data[0]['t']
    analyse(data, acks, f"client port {cport}, {data[-1]['t'] - t0:.0f} s")
