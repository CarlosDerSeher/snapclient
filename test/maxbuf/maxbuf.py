#!/usr/bin/env python3
"""Find the largest server buffer a board can play.

For each buffer (ascending), restart the test server (../netsim/testserver.sh)
with it, let the board reconnect and play for --dwell seconds, then judge that
part of the board's log:

  ok       pool built, playback synced, no hard resync, no allocation error
  FAIL     anything else (reason given)

The board's log comes from its serial port (--serial, needs pyserial) or from a
file another tool writes (--log FILE without --serial). Build the firmware with
CONFIG_SNAPCLIENT_HEAP_DIAG=y to also get the minimum free heap.

Example:
  test/maxbuf/maxbuf.py --serial /dev/ttyUSB0 800 850 900 950
"""

import argparse
import os
import re
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
NETSIM = os.path.join(HERE, "..", "netsim")
sys.path.insert(0, NETSIM)
import client  # noqa: E402  (test/netsim/client.py)

ANSI = re.compile(r"\x1b\[[0-9;]*m")


def serial_logger(port, path, stop):
    import serial  # pyserial

    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.2
    s.dtr = s.rts = False  # set before open, or the board is held in reset
    s.open()
    with open(path, "ab") as f:
        while not stop.is_set():
            data = s.read(4096)
            if data:
                f.write(data)
                f.flush()
    s.close()


def judge(seg):
    seg = ANSI.sub("", seg)
    r = {}
    pool = re.findall(r"PCMPOOL: (\d+) chunks of (\d+) B \(\d+ KB\)(?: needed, only (\d+) fit)?",
                      seg)
    r["pool"] = "-"
    r["pool_failed"] = False
    if pool:
        need, size, fit = pool[-1]
        r["pool"] = f"{need} x {size} B" + (f" ({fit} fit)" if fit else "")
        r["pool_failed"] = bool(fit)
    sync = seg.find("initial sync age")
    r["synced"] = sync >= 0
    r["resyncs"] = len(re.findall(r"RESYNCING HARD", seg[sync:])) if sync >= 0 else 0
    r["alloc_errors"] = len(re.findall(
        r"couldn't get memory|failed to allocate|Failed to allocate|no memory", seg))
    r["crash"] = bool(re.search(r"Guru Meditation|abort\(\)|rst:0x", seg))
    mins = [int(m) for m in re.findall(r"HEAPDIAG: 8BIT free \d+ largest \d+ min (\d+)", seg)]
    r["heap_min"] = min(mins) if mins else None
    reasons = []
    if r["crash"]:
        reasons.append("crash/reset")
    if r["pool_failed"]:
        reasons.append("pool doesn't fit")
    if not r["synced"]:
        reasons.append("never synced")
    if r["resyncs"]:
        reasons.append(f"{r['resyncs']} hard resyncs")
    if r["alloc_errors"]:
        reasons.append(f"{r['alloc_errors']} allocation errors")
    r["verdict"] = "FAIL: " + ", ".join(reasons) if reasons else "ok"
    return r


def wait_for_board(server, which, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            g, c = client.find_board(server, which)
            if c:
                return g, c
        except OSError:
            pass  # server still starting
        time.sleep(1)
    return None, None


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                formatter_class=argparse.RawDescriptionHelpFormatter,
                                epilog=__doc__.split("\n", 2)[2])
    p.add_argument("buffers", nargs="+", type=int, help="server buffers in ms")
    p.add_argument("--serial", help="board's serial port, e.g. /dev/ttyUSB0")
    p.add_argument("--log", help="board log file (written with --serial, read otherwise)")
    p.add_argument("--stream", default="flac44", help="stream to play (default flac44)")
    p.add_argument("--dwell", type=int, default=45, help="seconds per buffer (default 45)")
    p.add_argument("--server", default="127.0.0.1:1780", help="test server JSON-RPC")
    p.add_argument("--client", help="client ID, name or IP (default: the connected ESP32)")
    a = p.parse_args()

    if not a.serial and not a.log:
        p.error("give --serial PORT, or --log FILE that another tool writes")
    log = a.log or time.strftime("maxbuf-%Y%m%d-%H%M%S.log")
    stop = threading.Event()
    if a.serial:
        threading.Thread(target=serial_logger, args=(a.serial, log, stop), daemon=True).start()
        print(f"logging {a.serial} to {log}; opening the port resets the board")
        time.sleep(8)

    server = None
    results = []
    try:
        for buf in sorted(a.buffers):
            if server:
                server.terminate()
                server.wait()
            start = os.path.getsize(log) if os.path.exists(log) else 0
            server = subprocess.Popen([os.path.join(NETSIM, "testserver.sh"), str(buf)],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            g, c = wait_for_board(a.server, a.client, 60)
            if not c:
                print(f"{buf:5d} ms  FAIL: board didn't connect")
                results.append((buf, False))
                continue
            if g["stream_id"] != a.stream or c["config"]["volume"]["muted"]:
                time.sleep(5)  # a switch right after connecting can get lost
                client.rpc(a.server, "Client.SetVolume",
                           {"id": c["id"], "volume": {"muted": False, "percent": 1}})
                client.rpc(a.server, "Group.SetStream", {"id": g["id"], "stream_id": a.stream})
            time.sleep(a.dwell)
            with open(log, "rb") as f:
                f.seek(start)
                r = judge(f.read().decode("utf-8", "replace"))
            heap = f"{r['heap_min'] / 1000:5.1f} KB" if r["heap_min"] else "     -  "
            print(f"{buf:5d} ms  pool {r['pool']:20s} heap min {heap}  {r['verdict']}", flush=True)
            results.append((buf, r["verdict"] == "ok"))
    finally:
        if server:
            server.terminate()
            server.wait()
        stop.set()

    ok = [b for b, good in results if good]
    print(f"largest buffer that played cleanly: {max(ok)} ms" if ok else "no buffer played cleanly")


if __name__ == "__main__":
    main()
