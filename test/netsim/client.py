#!/usr/bin/env python3
"""Control a snapclient board for netsim runs.

On the board (its web API):
  client.py point BOARD HOST [PORT]   use the server at HOST:PORT, mDNS off, restart
  client.py restore BOARD             back to the default server settings, restart

On the snapserver (JSON-RPC, --server, default 127.0.0.1:1780):
  client.py status                    list the clients
  client.py stream NAME               switch the board's group to stream NAME and
                                      set its volume (default 1 %)

The board is found among the server's clients as the only connected ESP32
(--client picks one by ID, name or IP). Through netsim_proxy.py the server sees
the proxy's address, not the board's, so the IP is no help there.

A muted client doesn't start its player, so `stream` also unmutes it. A switch
right after the board connects can get lost: `stream` waits --settle seconds
after the board shows up. Check the chunk size in the board's log
(FLAC 44.1 kHz: "PCMPOOL: ... chunks of 4608 B").
"""

import argparse
import json
import sys
import time
import urllib.request


def board_request(board, method, path):
    req = urllib.request.Request(f"http://{board}{path}", method=method)
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.read().decode(errors="replace")
    except OSError as e:
        sys.exit(f"{board}{path}: {e}")


def board_restart(board):
    try:
        board_request(board, "POST", "/restart")
    except SystemExit:
        pass  # the board may close the connection while restarting
    print(f"{board}: restarting")


def cmd_point(a):
    for param, value in (("snapserver_host", a.host),
                         ("snapserver_port", a.port),
                         ("snapserver_use_mdns", "0")):
        board_request(a.board, "POST", f"/post?param={param}&value={value}")
        time.sleep(0.5)
    print(f"{a.board}: server {a.host}:{a.port}, mDNS off")
    board_restart(a.board)


def cmd_restore(a):
    for param in ("snapserver_host", "snapserver_port", "snapserver_use_mdns"):
        try:
            board_request(a.board, "DELETE", f"/delete?param={param}")
        except SystemExit as e:
            # the board answers 500 when the setting isn't stored at all
            print(f"{param}: {e} (not set?), continuing")
        time.sleep(0.5)
    print(f"{a.board}: server settings back to defaults")
    board_restart(a.board)


def rpc(server, method, params=None):
    q = {"id": 1, "jsonrpc": "2.0", "method": method}
    if params:
        q["params"] = params
    req = urllib.request.Request(f"http://{server}/jsonrpc", json.dumps(q).encode(),
                                 {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=5) as r:
        reply = json.loads(r.read())
    if "error" in reply:
        sys.exit(f"{method}: {reply['error']}")
    return reply["result"]


def clients(server):
    for g in rpc(server, "Server.GetStatus")["server"]["groups"]:
        for c in g["clients"]:
            yield g, c


def find_board(server, which):
    found = []
    for g, c in clients(server):
        if not c["connected"]:
            continue
        if which:
            ip = c["host"]["ip"].removeprefix("::ffff:")
            if which in (c["id"], c["host"]["name"], c["config"]["name"], c["host"]["ip"], ip):
                found.append((g, c))
        elif c["host"]["os"] == "esp32":
            found.append((g, c))
    if len(found) > 1:
        sys.exit("several boards connected, pick one with --client: "
                 + ", ".join(c["id"] for _, c in found))
    return found[0] if found else (None, None)


def cmd_status(a):
    for g, c in clients(a.server):
        v = c["config"]["volume"]
        print(f"{c['id']:20s} {c['host']['os']:8s} {c['host']['ip']:24s} "
              f"{'connected' if c['connected'] else 'gone':9s} stream {g['stream_id']:8s} "
              f"volume {v['percent']}{' muted' if v['muted'] else ''}")


def cmd_stream(a):
    streams = [s["id"] for s in rpc(a.server, "Server.GetStatus")["server"]["streams"]]
    if a.name not in streams:
        sys.exit(f"no stream {a.name}, the server has: {' '.join(streams)}")
    g, c = find_board(a.server, a.client)
    if not c:
        print(f"waiting up to {a.wait} s for the board to connect")
        deadline = time.time() + a.wait
        while not c and time.time() < deadline:
            time.sleep(1)
            g, c = find_board(a.server, a.client)
        if not c:
            sys.exit("board not connected to the server")
        time.sleep(a.settle)
    rpc(a.server, "Client.SetVolume",
        {"id": c["id"], "volume": {"muted": False, "percent": a.volume}})
    rpc(a.server, "Group.SetStream", {"id": g["id"], "stream_id": a.name})
    print(f"{time.strftime('%H:%M:%S')} {c['id']}: stream {a.name}, volume {a.volume} %")


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                formatter_class=argparse.RawDescriptionHelpFormatter,
                                epilog=__doc__.split("\n", 2)[2])
    p.add_argument("--server", default="127.0.0.1:1780", help="snapserver HTTP JSON-RPC")
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("point", help="point the board at a server")
    s.add_argument("board", help="board address")
    s.add_argument("host", help="server (or proxy) address as the board sees it")
    s.add_argument("port", nargs="?", default="1706", help="default 1706 (the proxy)")
    s.set_defaults(func=cmd_point)

    s = sub.add_parser("restore", help="default server settings on the board")
    s.add_argument("board", help="board address")
    s.set_defaults(func=cmd_restore)

    s = sub.add_parser("status", help="list the server's clients")
    s.set_defaults(func=cmd_status)

    s = sub.add_parser("stream", help="switch the board's stream")
    s.add_argument("name", help="stream ID, e.g. flac44")
    s.add_argument("--client", help="client ID, name or IP (default: the connected ESP32)")
    s.add_argument("--volume", type=int, default=1, help="percent, default 1")
    s.add_argument("--wait", type=int, default=60, help="seconds to wait for the board")
    s.add_argument("--settle", type=float, default=5,
                   help="seconds to wait after the board connects (default 5)")
    s.set_defaults(func=cmd_stream)

    a = p.parse_args()
    a.func(a)


if __name__ == "__main__":
    main()
