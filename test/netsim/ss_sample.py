#!/usr/bin/env python3
"""Sample the server's TCP state of the connection to the board every 20 ms, on the server host.

usage: ss_sample.py OUT SECONDS BOARD_IP [PORT]
Writes one line per sample: the time and `ss -tinmH` (congestion window, retransmit timeout and
backoff, unacked, lastack, retransmits, ...). PORT is the server's stream port (default 1704).
Start it together with the run; analyse with ss_summary.py.
"""
import subprocess
import sys
import time

out = open(sys.argv[1], 'w')
end = time.time() + float(sys.argv[2])
port = sys.argv[4] if len(sys.argv) > 4 else '1704'
while time.time() < end:
    t = time.time()
    r = subprocess.run(['ss', '-tinmH', 'state', 'established', f'( sport = :{port} )',
                        'dst', sys.argv[3]], capture_output=True, text=True).stdout
    out.write(f"{t:.3f} " + ' '.join(r.split()) + "\n")
    time.sleep(max(0, 0.02 - (time.time() - t)))
