#!/usr/bin/env python3
"""TCP proxy between snapclient and snapserver that simulates bad Wi-Fi.

The client connects to the proxy instead of the server. While a stall is active, the proxy keeps
reading from both ends but holds the data, then sends it in one burst when the stall ends: to the
client this looks like a Wi-Fi outage followed by the backlog arriving at full speed.

Commands (from --control FILE, a --schedule entry, or stdin with --stdin):
  stall SECONDS                    hold both directions for SECONDS
  jitter MIN MAX MEAN_GAP SEED     random stalls of uniform(MIN, MAX) s with exponential gaps of
                                   mean MEAN_GAP s; a fixed SEED gives the same sequence every run
  jitter off                       stop random stalls
  rate KBYTES_PER_S | rate off     limit throughput in each direction (slow link)
  delay SECONDS | delay off        hold each direction for SECONDS (one way, RTT = 2 x SECONDS)
  quit                             exit

Examples:
  # board -> :1706 -> snapserver on this host :1704, commands written to /tmp/netsim.ctl
  netsim_proxy.py --listen 1706 --target 127.0.0.1:1704 --control /tmp/netsim.ctl
  echo "stall 2.5" > /tmp/netsim.ctl

  # unattended: random stalls from t=60 s to t=300 s, then exit at t=330 s
  netsim_proxy.py --listen 1706 --target 127.0.0.1:1704 \\
      --schedule "60 jitter 0.1 0.7 3 1" "300 jitter off" "330 quit"

Point the client at the proxy (snapserver host = this machine, port = listen port, mDNS off).
Every event is logged with a timestamp to stdout, so the log can be lined up with the client log.
"""
import argparse
import os
import random
import socket
import sys
import threading
import time


class NetSim:
    def __init__(self, log=print):
        self.stall_until = 0.0
        self.rate = 0  # bytes per second, 0 = unlimited
        self.delay = 0.0  # one-way delay in seconds, per direction
        self._jitter_gen = 0
        self._log = log
        self.stop = threading.Event()

    def log(self, *a):
        self._log(time.strftime('%H:%M:%S'), *a)

    def stalled(self):
        return time.time() < self.stall_until

    def stall(self, seconds):
        self.stall_until = max(self.stall_until, time.time() + seconds)

    def _jitter(self, gen, lo, hi, gap, seed):
        rnd = random.Random(seed)
        while self._jitter_gen == gen and not self.stop.is_set():
            time.sleep(rnd.expovariate(1.0 / gap))
            if self._jitter_gen != gen:
                break
            d = rnd.uniform(lo, hi)
            self.stall(d)
            self.log('jitter stall %.3f' % d)
            time.sleep(d)

    def command(self, line):
        a = line.split()
        if not a:
            return
        try:
            if a[0] == 'stall':
                self.stall(float(a[1]))
                self.log('stall', a[1])
            elif a[0] == 'jitter':
                self._jitter_gen += 1
                if a[1] != 'off':
                    args = (self._jitter_gen, float(a[1]), float(a[2]), float(a[3]), int(a[4]))
                    threading.Thread(target=self._jitter, args=args, daemon=True).start()
                self.log('jitter', *a[1:])
            elif a[0] == 'delay':
                self.delay = 0.0 if a[1] == 'off' else float(a[1])
                self.log('delay', a[1])
            elif a[0] == 'rate':
                self.rate = 0 if a[1] == 'off' else int(float(a[1]) * 1000)
                self.log('rate', a[1])
            elif a[0] == 'quit':
                self.log('quit')
                self.stop.set()
            else:
                self.log('unknown command:', line.strip())
        except (IndexError, ValueError):
            self.log('bad command:', line.strip())

    def pump(self, src, dst, name):
        """Forward src -> dst. A reader thread always drains src, so stalls hold data here."""
        buf = bytearray()
        arrivals = []  # (time, end offset in buf) per received block, for delay
        lock = threading.Lock()
        eof = threading.Event()

        def reader():
            while True:
                try:
                    d = src.recv(65536)
                except OSError:
                    d = b''
                if not d:
                    eof.set()
                    return
                with lock:
                    buf.extend(d)
                    arrivals.append((time.time(), len(buf)))

        threading.Thread(target=reader, daemon=True).start()
        was_stalled = False
        budget, last = 0.0, time.time()
        while not self.stop.is_set():
            now = time.time()
            if self.stalled():
                was_stalled = True
            else:
                with lock:
                    n = len(buf)
                    if self.delay:
                        # only bytes that have been held for the delay are due
                        n = 0
                        for t, end in arrivals:
                            if t + self.delay > now:
                                break
                            n = end
                    if self.rate:
                        budget = min(budget + (now - last) * self.rate, self.rate * 0.05)
                        n = min(n, int(budget))
                        budget -= n
                    out = bytes(buf[:n])
                    del buf[:n]
                    arrivals[:] = [(t, end - n) for t, end in arrivals if end > n]
                    left = len(buf)
                if was_stalled:
                    self.log('%s burst %d B' % (name, len(out) + left))
                    was_stalled = False
                if out:
                    try:
                        dst.sendall(out)
                    except OSError:
                        break
                elif eof.is_set() and not left:
                    break
            last = now
            time.sleep(0.002)
        for s in (src, dst):
            try:
                s.close()
            except OSError:
                pass


def parse_addr(s, default_host):
    host, _, port = s.rpartition(':')
    return (host or default_host), int(port)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--listen', default='1706', help='[HOST:]PORT to listen on (default 0.0.0.0:1706)')
    ap.add_argument('--target', default='127.0.0.1:1704', help='snapserver HOST:PORT (default 127.0.0.1:1704)')
    ap.add_argument('--control', help='file polled for commands; removed after reading')
    ap.add_argument('--stdin', action='store_true', help='read commands from stdin')
    ap.add_argument('--schedule', nargs='*', default=[], metavar='"T CMD"',
                    help='run CMD T seconds after start, e.g. "60 jitter 0.1 0.7 3 1"')
    o = ap.parse_args()

    sim = NetSim(log=lambda *a: print(*a, flush=True))
    lhost, lport = parse_addr(o.listen, '0.0.0.0')
    target = parse_addr(o.target, '127.0.0.1')
    t0 = time.time()

    def scheduler():
        for t, cmd in sorted((float(e.split(None, 1)[0]), e.split(None, 1)[1]) for e in o.schedule):
            while time.time() < t0 + t:
                if sim.stop.wait(0.05):
                    return
            sim.command(cmd)

    def control_file():
        while not sim.stop.wait(0.05):
            try:
                with open(o.control) as f:
                    lines = f.read().splitlines()
                os.remove(o.control)
            except FileNotFoundError:
                continue
            for line in lines:
                sim.command(line)

    def control_stdin():
        for line in sys.stdin:
            sim.command(line)

    if o.schedule:
        threading.Thread(target=scheduler, daemon=True).start()
    if o.control:
        threading.Thread(target=control_file, daemon=True).start()
    if o.stdin:
        threading.Thread(target=control_stdin, daemon=True).start()

    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind((lhost, lport))
    ls.listen(4)
    ls.settimeout(0.2)
    sim.log('listening on %s:%d -> %s:%d' % (lhost, lport, *target))
    while not sim.stop.is_set():
        try:
            c, addr = ls.accept()
        except socket.timeout:
            continue
        sim.log('connection from %s:%d' % addr)
        try:
            s = socket.create_connection(target, timeout=5)
            s.settimeout(None)
        except OSError as e:
            sim.log('server not reachable:', e)
            c.close()
            continue
        for x in (c, s):
            x.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        threading.Thread(target=sim.pump, args=(c, s, 'to server'), daemon=True).start()
        threading.Thread(target=sim.pump, args=(s, c, 'to client'), daemon=True).start()
    ls.close()


if __name__ == '__main__':
    main()
