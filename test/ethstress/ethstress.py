#!/usr/bin/env python3
"""Ethernet stress test: stack peaks under load, Wi-Fi fallback and takeover.

Runs one board through all test streams while the web UI is queried and the
volume changes, pulls and plugs the Ethernet "cable" during playback (needs the
link test hook), switches to static IP and back, and ends with an OTA upload.
Reads the serial log throughout and prints the lowest free stack per task
(HEAPDIAG), crashes and timeouts. See README.md.
"""
import argparse
import json
import random
import re
import socket
import sys
import threading
import time
import urllib.error
import urllib.request

try:
    import serial
except ImportError:
    sys.exit("needs pyserial (pip install pyserial)")

LINK_TEST_PORT = 9997
CRASH = re.compile(r"stack overflow|Guru Meditation|abort\(\)|panic'ed|Backtrace:")
STACK = re.compile(r"HEAPDIAG: stack (\S+)\s+hwm\s+(\d+)")


class Board:
    """Serial log reader; resets the board through RTS."""

    def __init__(self, port, logfile):
        self.lines = []
        self.lock = threading.Lock()
        self.t0 = time.time()
        self.log = open(logfile, "w")
        self.ser = serial.Serial()
        self.ser.port, self.ser.baudrate, self.ser.timeout = port, 115200, 0.1
        # RTS asserted holds the ESP32 in reset through the auto-reset circuit
        self.ser.dtr = self.ser.rts = False
        self.ser.open()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        while True:
            raw = self.ser.readline()
            if raw:
                line = re.sub(r"\x1b\[[0-9;]*m", "", raw.decode(errors="replace").rstrip())
                with self.lock:
                    self.lines.append(line)
                self.log.write("%8.3f %s\n" % (time.time() - self.t0, line))
                self.log.flush()

    def reset(self):
        self.ser.rts = True
        time.sleep(0.1)
        self.ser.rts = False

    def mark(self, text):
        self.log.write("%8.3f ######## %s\n" % (time.time() - self.t0, text))
        self.log.flush()
        print("%6.1f %s" % (time.time() - self.t0, text), flush=True)

    def pos(self):
        with self.lock:
            return len(self.lines)

    def wait(self, pattern, timeout, since=None):
        """Index of the first line matching pattern after since, or None."""
        end = time.time() + timeout
        i = self.pos() if since is None else since
        while time.time() < end:
            with self.lock:
                for k in range(i, len(self.lines)):
                    if re.search(pattern, self.lines[k]):
                        return k
                i = len(self.lines)
            time.sleep(0.2)
        return None

    def seen(self, pattern):
        with self.lock:
            return any(re.search(pattern, l) for l in self.lines)


class Test:
    def __init__(self, a):
        self.a = a
        self.board = Board(a.serial, a.log)
        self.web = "http://" + a.board
        self.timeouts = 0
        self.web_ok = self.web_err = 0
        self.load = threading.Event()
        self.done = threading.Event()

    # --- snapserver JSON-RPC -------------------------------------------
    def rpc(self, method, params=None):
        req = urllib.request.Request(
            "http://%s/jsonrpc" % self.a.server,
            json.dumps({"id": 1, "jsonrpc": "2.0", "method": method,
                        "params": params or {}}).encode())
        return json.load(urllib.request.urlopen(req, timeout=3))

    def client(self):
        """(group, client) of the board on the server, or (None, None)."""
        for g in self.rpc("Server.GetStatus")["result"]["server"]["groups"]:
            for c in g["clients"]:
                if self.a.mac:
                    if c["host"]["mac"].lower() == self.a.mac.lower():
                        return g, c
                elif c["connected"] and c["host"]["ip"].split(":")[-1] == self.a.board:
                    return g, c
        return None, None

    def stream(self, sid):
        g, _ = self.client()
        if g:
            self.rpc("Group.SetStream", {"id": g["id"], "stream_id": sid})

    def volume(self, percent, muted=False):
        _, c = self.client()
        if c:
            self.rpc("Client.SetVolume", {"id": c["id"],
                                          "volume": {"muted": muted, "percent": percent}})

    # --- board web UI ---------------------------------------------------
    def http(self, method, path, data=None, timeout=3, tries=5):
        for _ in range(tries):
            try:
                req = urllib.request.Request(self.web + path, data=data, method=method)
                return urllib.request.urlopen(req, timeout=timeout).read()
            except (urllib.error.URLError, OSError):
                time.sleep(2)
        return None

    def general(self):
        raw = self.http("GET", "/capabilities?tab=general")
        return json.loads(raw) if raw else {}

    # --- background load --------------------------------------------------
    def _web_load(self):
        paths = ["/", "/capabilities?tab=status", "/capabilities?tab=general",
                 "/capabilities?tab=dsp", "/styles.css"]
        while not self.done.is_set():
            if not self.load.is_set():
                time.sleep(0.2)
                continue
            try:
                urllib.request.urlopen(self.web + random.choice(paths), timeout=3).read()
                self.web_ok += 1
            except (urllib.error.URLError, OSError):
                self.web_err += 1
            time.sleep(self.a.web_interval)

    def _volume_load(self):
        v = self.a.volume
        while not self.done.is_set():
            if self.load.is_set():
                try:
                    self.volume(v)
                    v = self.a.volume + 1 if v == self.a.volume else self.a.volume
                except (urllib.error.URLError, OSError):
                    pass
            time.sleep(2)

    # --- steps ------------------------------------------------------------
    def expect(self, pattern, timeout, since, what):
        if self.board.wait(pattern, timeout, since) is None:
            self.timeouts += 1
            self.board.mark("TIMEOUT: " + what)
            return False
        return True

    def play(self, sid, hold):
        self.board.mark("stream " + sid)
        g, _ = self.client()
        i = self.board.pos()
        self.stream(sid)
        if g and g["stream_id"] != sid:  # already playing it: no new start
            self.expect(r"initial sync age|doesn't fit", 30, i, sid + " didn't start")
        time.sleep(hold)

    def link(self, cmd, pattern):
        for _ in range(5):
            i = self.board.pos()
            socket.socket(socket.AF_INET, socket.SOCK_DGRAM).sendto(
                cmd.encode(), (self.a.board, LINK_TEST_PORT))
            if self.board.wait(pattern, 3, i) is not None:
                return True
        self.timeouts += 1
        self.board.mark("TIMEOUT: link test hook didn't answer '%s'" % cmd)
        return False

    def cable_cycle(self, name, sid):
        b = self.board
        b.mark(name + ": cable out")
        i = b.pos()
        if self.link("d", "link test: PHY down"):
            self.expect("initial sync age", 40, i, "playback over Wi-Fi")
            time.sleep(8)
        b.mark(name + ": cable in")
        i = b.pos()
        if self.link("u", "link test: PHY up"):
            self.expect("deferring", 40, i, "Ethernet up, takeover deferred")
            time.sleep(5)
        b.mark(name + ": playback stops")
        i = b.pos()
        self.stream("idle")
        self.expect("eth_on_playback_stopped: mac_unify=1", 20, i, "deferred takeover")
        self.expect("netconn connected using ETH_DEF", 30, i, "reconnect over Ethernet")
        time.sleep(4)
        self.play(sid, 8)

    def restart(self, what):
        self.load.clear()
        self.board.mark(what)
        i = self.board.pos()
        self.http("POST", "/restart")
        self.expect("initial sync age", 90, i, what + ": playback")
        time.sleep(3)
        self.load.set()

    def run(self):
        a, b = self.a, self.board
        g, c = self.client()
        saved_stream = g["stream_id"] if g else None
        saved_volume = c["config"]["volume"] if c else None
        saved_eth = self.general()
        static_ip = a.static_ip or a.board
        threading.Thread(target=self._web_load, daemon=True).start()
        threading.Thread(target=self._volume_load, daemon=True).start()
        try:
            self.volume(a.volume)
            self.stream(a.streams[0])
            b.mark("boot")
            self.first_line = b.pos()  # opening the port may have reset it too
            self.expected_boots = 1 + (0 if a.no_static else 2) + (1 if a.ota else 0)
            b.reset()
            self.expect("initial sync age", 60, 0, "playback after boot")
            time.sleep(3)
            cable = not a.no_cable
            if cable and not b.seen("link test hook on UDP"):
                b.mark("no link test hook in the firmware (SNAPCLIENT_ETH_LINK_TEST): "
                       "skipping cable steps")
                cable = False
            self.load.set()
            for sid in a.streams:
                self.play(sid, a.hold)
            if cable:
                for sid in ("opus", "pcm"):
                    self.play(sid, 5)
                    self.cable_cycle("dhcp " + sid, sid)
            if not a.no_static:
                for kv in ("eth_static_ip&value=" + static_ip,
                           "eth_gateway&value=" + a.gateway,
                           "eth_dns&value=" + a.gateway, "eth_mode&value=2"):
                    self.http("POST", "/post?param=" + kv)
                self.restart("static: restart")
                self.play("noise44", 10)
                if cable:
                    self.cable_cycle("static noise44", "noise44")
                self.restore_eth(saved_eth)
                self.restart("dhcp: restart")
            if a.ota:
                self.play(a.streams[0], 5)
                image = open(a.ota, "rb").read()
                b.mark("OTA upload, %d bytes" % len(image))
                i, t = b.pos(), time.time()
                r = self.http("POST", "/api/ota/upload", data=image, timeout=180, tries=1)
                b.mark("OTA done after %.1f s: %s" % (time.time() - t, r))
                self.load.clear()
                self.expect("rst:", 30, i, "reboot after OTA")
                self.expect("initial sync age", 90, i, "playback after OTA")
                time.sleep(3)
            self.load.clear()
            b.wait("==== end periodic", 70)
        finally:
            self.done.set()
            self.restore_eth(saved_eth)
            try:
                if saved_stream:
                    self.stream(saved_stream)
                if saved_volume:
                    self.volume(saved_volume["percent"], saved_volume["muted"])
            except (urllib.error.URLError, OSError):
                print("couldn't restore stream/volume on the server")
            b.mark("done")
        return self.summary()

    def restore_eth(self, saved):
        """Back to the Ethernet settings the board had before the test."""
        if not saved or self.general() == saved:
            return
        self.http("POST", "/post?param=eth_mode&value=%d" % saved.get("eth_mode", 1))
        for key in ("eth_static_ip", "eth_gateway", "eth_dns"):
            if saved.get(key):
                self.http("POST", "/post?param=%s&value=%s" % (key, saved[key]))
            else:
                self.http("DELETE", "/delete?param=" + key)

    def summary(self):
        b = self.board
        with b.lock:
            lines = list(b.lines)
        low = {}
        for l in lines:
            m = STACK.search(l)
            if m:
                low[m.group(1)] = min(low.get(m.group(1), 1 << 30), int(m.group(2)))
        crashes = [l for l in lines if CRASH.search(l)]
        print("\nLowest free stack per task (bytes, HEAPDIAG hwm):")
        for task, free in sorted(low.items(), key=lambda kv: kv[1]):
            # ipc0/ipc1 are IDF tasks with a fixed small stack
            low_mark = free < 512 and not task.startswith("ipc")
            print("  %-16s %6d%s" % (task, free, "   <-- low" if low_mark else ""))
        if not low:
            print("  none: build with CONFIG_SNAPCLIENT_HEAP_DIAG=y")
        boots = sum("rst:" in l for l in lines[getattr(self, "first_line", 0):])
        print("\nboots %d (expected %d), playback starts %d, pool didn't fit %d, "
              "web requests %d ok %d failed"
              % (boots, getattr(self, "expected_boots", 0),
                 sum("initial sync age" in l for l in lines),
                 sum("doesn't fit" in l for l in lines), self.web_ok, self.web_err))
        print("timeouts %d, crashes %d" % (self.timeouts, len(crashes)))
        for l in crashes[:5]:
            print("  " + l)
        print("serial log: " + self.a.log)
        return 1 if crashes else 0


def default_gateway():
    try:
        for line in open("/proc/net/route").readlines()[1:]:
            f = line.split()
            if f[1] == "00000000":
                return socket.inet_ntoa(bytes.fromhex(f[2])[::-1])
    except OSError:
        pass
    return None


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                formatter_class=argparse.RawDescriptionHelpFormatter,
                                epilog=__doc__.split("\n", 2)[2])
    p.add_argument("--serial", required=True, help="serial port of the board, e.g. /dev/ttyUSB1")
    p.add_argument("--board", required=True, help="board IP address")
    p.add_argument("--mac", help="board MAC as the server lists it (default: find by IP)")
    p.add_argument("--server", default="127.0.0.1:1780", help="snapserver HTTP JSON-RPC")
    p.add_argument("--streams", default="flac44,pcm,opus,noise44,flac48",
                   help="streams to play, comma separated (first one also before OTA)")
    p.add_argument("--hold", type=float, default=20, help="seconds per stream")
    p.add_argument("--volume", type=int, default=1, help="volume in percent (default 1)")
    p.add_argument("--web-interval", type=float, default=0.15,
                   help="seconds between web UI requests")
    p.add_argument("--no-cable", action="store_true", help="skip the cable out/in steps")
    p.add_argument("--no-static", action="store_true", help="skip the static IP steps")
    p.add_argument("--static-ip", help="static IP to test (default: the board IP)")
    p.add_argument("--gateway", default=default_gateway(),
                   help="gateway and DNS for the static IP (default: this PC's gateway)")
    p.add_argument("--ota", metavar="BIN", help="firmware image to upload at the end")
    p.add_argument("--log", help="serial log file (default ethstress-BOARD.log)")
    a = p.parse_args()
    a.streams = a.streams.split(",")
    a.log = a.log or "ethstress-%s.log" % a.board
    if not a.no_static and not a.gateway:
        p.error("no default gateway found: pass --gateway or --no-static")
    sys.exit(Test(a).run())


if __name__ == "__main__":
    main()
