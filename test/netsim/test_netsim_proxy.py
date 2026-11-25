#!/usr/bin/env python3
"""Host test for netsim_proxy.py (no board needed): python3 test/netsim/test_netsim_proxy.py"""
import os
import socket
import subprocess
import sys
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))


def free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    p = s.getsockname()[1]
    s.close()
    return p


class EchoServer:
    def __init__(self):
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(('127.0.0.1', 0))
        self.sock.listen(1)
        self.port = self.sock.getsockname()[1]
        threading.Thread(target=self.run, daemon=True).start()

    def run(self):
        c, _ = self.sock.accept()
        while True:
            d = c.recv(65536)
            if not d:
                break
            c.sendall(d)
        c.close()


class ProxyTest(unittest.TestCase):
    def setUp(self):
        self.echo = EchoServer()
        self.port = free_port()
        self.ctl = os.path.join(HERE, '.test-%d.ctl' % self.port)
        self.proc = subprocess.Popen(
            [sys.executable, os.path.join(HERE, 'netsim_proxy.py'), '--listen', '127.0.0.1:%d' % self.port,
             '--target', '127.0.0.1:%d' % self.echo.port, '--control', self.ctl],
            stdout=subprocess.PIPE, text=True)
        self.proc.stdout.readline()  # "listening on ..."
        self.c = socket.create_connection(('127.0.0.1', self.port))
        self.c.settimeout(5)

    def tearDown(self):
        self.c.close()
        self.echo.sock.close()
        self.proc.kill()
        self.proc.wait()
        self.proc.stdout.close()
        if os.path.exists(self.ctl):
            os.remove(self.ctl)

    def command(self, cmd):
        with open(self.ctl, 'w') as f:
            f.write(cmd + '\n')
        while os.path.exists(self.ctl):
            time.sleep(0.01)
        time.sleep(0.05)

    def roundtrip(self, data):
        t = time.time()
        self.c.sendall(data)
        got = b''
        while len(got) < len(data):
            got += self.c.recv(65536)
        self.assertEqual(got, data)
        return time.time() - t

    def test_passthrough(self):
        self.assertLess(self.roundtrip(b'x' * 100000), 1.0)

    def test_stall_holds_then_bursts(self):
        self.command('stall 0.6')
        self.assertGreater(self.roundtrip(b'abc' * 1000), 0.4)
        self.assertLess(self.roundtrip(b'after'), 0.3)

    def test_rate_limit(self):
        self.command('rate 100')  # 100 KB/s each way -> 50 KB takes ~0.5 s
        self.assertGreater(self.roundtrip(b'y' * 50000), 0.35)
        self.command('rate off')
        self.assertLess(self.roundtrip(b'y' * 50000), 0.3)


class BackpressureTest(unittest.TestCase):
    """A client that doesn't read must stop the server: the proxy holds at most --max-buffer."""

    def sent_while_client_sleeps(self, extra_args):
        srv = socket.socket()
        srv.bind(('127.0.0.1', 0))
        srv.listen(1)
        port = free_port()
        proc = subprocess.Popen(
            [sys.executable, os.path.join(HERE, 'netsim_proxy.py'), '--listen', '127.0.0.1:%d' % port,
             '--target', '127.0.0.1:%d' % srv.getsockname()[1]] + extra_args,
            stdout=subprocess.PIPE, text=True)
        try:
            proc.stdout.readline()
            c = socket.socket()
            c.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 64 * 1024)
            c.connect(('127.0.0.1', port))
            s, _ = srv.accept()
            s.setblocking(False)
            sent, end, chunk = 0, time.time() + 1.5, b'z' * 65536
            while time.time() < end:
                try:
                    sent += s.send(chunk)
                except BlockingIOError:
                    time.sleep(0.005)
            c.close()
            s.close()
            return sent
        finally:
            srv.close()
            proc.kill()
            proc.wait()
            proc.stdout.close()

    def test_bounded(self):
        # proxy 256 KB + its 64 KB socket buffers + the sender's send buffer (~2.5 MB on Linux)
        self.assertLess(self.sent_while_client_sleeps([]), 8 * 1024 * 1024)

    def test_unlimited(self):
        self.assertGreater(self.sent_while_client_sleeps(['--max-buffer', '0']), 64 * 1024 * 1024)


if __name__ == '__main__':
    unittest.main()
