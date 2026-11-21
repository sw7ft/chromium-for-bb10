#!/usr/bin/env python3
"""Minimal Chrome DevTools Protocol client (stdlib only).

The build host has no websocket package, so this implements just enough of
RFC 6455 for CDP: client handshake, masked text frames out, unmasked text
frames in (with fragmentation and ping handling).
"""
import base64
import json
import os
import socket
import struct
import time
import urllib.request


class CDP:
    def __init__(self, host, port, timeout=30):
        self.host, self.port = host, port
        self.sock = None
        self.next_id = 0
        self.events = []
        self.buf = b""
        self.timeout = timeout

    # ---- discovery -------------------------------------------------------
    def http_json(self, path):
        with urllib.request.urlopen(
            "http://%s:%d%s" % (self.host, self.port, path), timeout=10
        ) as r:
            return json.loads(r.read().decode())

    def wait_ready(self, seconds=90):
        end = time.time() + seconds
        last = None
        while time.time() < end:
            try:
                return self.http_json("/json/version")
            except Exception as e:  # not listening yet
                last = e
                time.sleep(1)
        raise RuntimeError("devtools endpoint never came up: %r" % (last,))

    def page_ws_path(self):
        for t in self.http_json("/json/list"):
            if t.get("type") == "page":
                url = t["webSocketDebuggerUrl"]
                return "/" + url.split("/", 3)[3]
        raise RuntimeError("no page target")

    # ---- websocket -------------------------------------------------------
    def connect(self, path):
        s = socket.create_connection((self.host, self.port), timeout=self.timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            "GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n" % (path, self.host, self.port, key)
        )
        s.sendall(req.encode())
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = s.recv(4096)
            if not chunk:
                raise RuntimeError("handshake closed")
            head += chunk
        hdr, _, rest = head.partition(b"\r\n\r\n")
        if b" 101 " not in hdr.split(b"\r\n")[0]:
            raise RuntimeError("handshake failed: %r" % hdr[:200])
        self.sock, self.buf = s, rest

    def _send_frame(self, opcode, payload):
        mask = os.urandom(4)
        n = len(payload)
        head = bytes([0x80 | opcode])
        if n < 126:
            head += bytes([0x80 | n])
        elif n < 65536:
            head += bytes([0x80 | 126]) + struct.pack(">H", n)
        else:
            head += bytes([0x80 | 127]) + struct.pack(">Q", n)
        masked = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
        self.sock.sendall(head + mask + masked)

    def _read(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("socket closed")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def _recv_message(self):
        data = b""
        while True:
            b0, b1 = self._read(2)
            fin, opcode = b0 & 0x80, b0 & 0x0F
            n = b1 & 0x7F
            if n == 126:
                (n,) = struct.unpack(">H", self._read(2))
            elif n == 127:
                (n,) = struct.unpack(">Q", self._read(8))
            payload = self._read(n)
            if opcode == 0x9:  # ping
                self._send_frame(0xA, payload)
                continue
            if opcode == 0x8:
                raise RuntimeError("websocket closed by peer")
            if opcode in (0x1, 0x2, 0x0):
                data += payload
                if fin:
                    return data.decode("utf-8", "replace")

    # ---- protocol --------------------------------------------------------
    def call(self, method, params=None, timeout=None):
        self.next_id += 1
        mid = self.next_id
        self._send_frame(
            0x1, json.dumps({"id": mid, "method": method, "params": params or {}}).encode()
        )
        self.sock.settimeout(timeout or self.timeout)
        while True:
            msg = json.loads(self._recv_message())
            if msg.get("id") == mid:
                if "error" in msg:
                    raise RuntimeError("%s: %s" % (method, msg["error"]))
                return msg.get("result", {})
            self.events.append(msg)

    def pump(self, seconds):
        """Collect events for up to `seconds`."""
        end = time.time() + seconds
        while True:
            left = end - time.time()
            if left <= 0:
                return
            self.sock.settimeout(left)
            try:
                self.events.append(json.loads(self._recv_message()))
            except socket.timeout:
                return

    def evaluate(self, expr, await_promise=False, timeout=None):
        r = self.call(
            "Runtime.evaluate",
            {"expression": expr, "returnByValue": True, "awaitPromise": await_promise},
            timeout=timeout,
        )
        if "exceptionDetails" in r:
            d = r["exceptionDetails"]
            raise RuntimeError(
                "JS exception: %s"
                % (d.get("exception", {}).get("description") or d.get("text"))
            )
        return r["result"].get("value")

    def take_events(self):
        ev, self.events = self.events, []
        return ev
