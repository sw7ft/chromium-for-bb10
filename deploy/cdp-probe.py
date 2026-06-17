#!/usr/bin/env python3.2
# Dependency-free CDP probe for headless content_shell, runs ON the device
# (Python 3.2). Validates: navigate -> captureScreenshot -> startScreencast
# frames -> input scroll -> screenshot. Writes images to /tmp and prints sizes.
import sys, os, json, socket, struct, base64, time

# Connect directly to the browser-level WS endpoint (the content_shell
# "DevTools listening on ws://..." URL) and drive targets via the Target domain;
# the HTTP /json discovery endpoints do not respond on this build.
WSURL = sys.argv[1] if len(sys.argv) > 1 else None
URL   = sys.argv[2] if len(sys.argv) > 2 else 'https://example.com'
OUT   = sys.argv[3] if len(sys.argv) > 3 else '/tmp'

def log(*a):
    sys.stdout.write('[cdp] ' + ' '.join(str(x) for x in a) + '\n')
    sys.stdout.flush()

class WS:
    def __init__(self, host, port, path):
        self.sock = socket.create_connection((host, port), 10)
        key = base64.b64encode(os.urandom(16)).decode('ascii')
        req = ('GET %s HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n'
               'Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n'
               'Sec-WebSocket-Version: 13\r\n\r\n') % (path, key)
        self.sock.sendall(req.encode('utf8'))
        self.buf = b''
        # read until end of handshake headers
        while b'\r\n\r\n' not in self.buf:
            d = self.sock.recv(4096)
            if not d:
                raise IOError('handshake closed')
            self.buf += d
        hdr, _, rest = self.buf.partition(b'\r\n\r\n')
        if b'101' not in hdr.split(b'\r\n')[0]:
            raise IOError('bad handshake: ' + hdr[:120].decode('latin1'))
        self.buf = rest
        self.next_id = 0

    def _fill(self, n):
        while len(self.buf) < n:
            d = self.sock.recv(65536)
            if not d:
                raise IOError('socket closed')
            self.buf += d

    def recv_frame(self):
        self._fill(2)
        b0 = self.buf[0]; b1 = self.buf[1]
        op = b0 & 0x0f
        ln = b1 & 0x7f
        idx = 2
        if ln == 126:
            self._fill(4); ln = int.from_bytes(self.buf[2:4], 'big'); idx = 4
        elif ln == 127:
            self._fill(10); ln = int.from_bytes(self.buf[2:10], 'big'); idx = 10
        self._fill(idx + ln)
        payload = self.buf[idx:idx+ln]
        self.buf = self.buf[idx+ln:]
        return (b0 & 0x80), op, payload

    def recv_message(self):
        data = b''
        while True:
            fin, op, payload = self.recv_frame()
            if op == 0x8:
                raise IOError('ws close')
            if op == 0x9:  # ping -> pong
                self._send_raw(0x8A, payload); continue
            if op == 0xA:
                continue
            data += payload
            if fin:
                return data

    def _send_raw(self, b0, payload):
        ln = len(payload)
        mask = os.urandom(4)
        hdr = bytearray([b0])
        if ln < 126:
            hdr.append(0x80 | ln)
        elif ln < 65536:
            hdr.append(0x80 | 126); hdr += ln.to_bytes(2, 'big')
        else:
            hdr.append(0x80 | 127); hdr += ln.to_bytes(8, 'big')
        masked = bytes(payload[i] ^ mask[i & 3] for i in range(ln))
        self.sock.sendall(bytes(hdr) + mask + masked)

    def send_json(self, obj):
        self._send_raw(0x81, json.dumps(obj).encode('utf8'))

    def call(self, method, params=None, on_event=None, session=None):
        self.next_id += 1
        mid = self.next_id
        env = {'id': mid, 'method': method, 'params': params or {}}
        if session:
            env['sessionId'] = session
        self.send_json(env)
        while True:
            msg = json.loads(self.recv_message().decode('utf8'))
            if msg.get('id') == mid:
                if 'error' in msg:
                    raise IOError(method + ' error: ' + json.dumps(msg['error']))
                return msg.get('result', {})
            if on_event:
                on_event(msg)

    def pump(self, seconds, on_event):
        end = time.time() + seconds
        self.sock.settimeout(0.5)
        try:
            while time.time() < end:
                try:
                    msg = json.loads(self.recv_message().decode('utf8'))
                except socket.timeout:
                    continue
                on_event(msg)
        finally:
            self.sock.settimeout(None)

def main():
    if not WSURL:
        log('FAIL: pass browser ws url as argv[1]'); sys.exit(1)
    rest = WSURL.split('://', 1)[1]
    hostport, _, path = rest.partition('/')
    h, _, p = hostport.partition(':')
    ws = WS(h, int(p or '80'), '/' + path)
    log('connected browser endpoint', WSURL)

    # Find or create a page target, then attach (flatten -> sessionId envelope).
    tgts = ws.call('Target.getTargets').get('targetInfos', [])
    page = None
    for t in tgts:
        if t.get('type') == 'page':
            page = t; break
    if not page:
        tid = ws.call('Target.createTarget', {'url': 'about:blank'})['targetId']
    else:
        tid = page['targetId']
    log('target', tid)
    session = ws.call('Target.attachToTarget',
                      {'targetId': tid, 'flatten': True})['sessionId']
    log('attached session', session)

    frames = [0]
    first_frame = [None]
    def on_event(msg):
        if msg.get('method') == 'Page.screencastFrame':
            frames[0] += 1
            if first_frame[0] is None:
                first_frame[0] = base64.b64decode(msg['params']['data'])
            try:
                ws.send_json({'id': 999000 + frames[0],
                              'method': 'Page.screencastFrameAck',
                              'params': {'sessionId': msg['params']['sessionId']},
                              'sessionId': session})
            except Exception:
                pass

    ws.call('Page.enable', session=session)
    ws.call('Runtime.enable', session=session)
    log('navigate ->', URL)
    ws.call('Page.navigate', {'url': URL}, session=session)
    ws.pump(8, on_event)

    log('captureScreenshot #1')
    r = ws.call('Page.captureScreenshot', {'format': 'png'}, on_event, session)
    png1 = base64.b64decode(r['data'])
    open(os.path.join(OUT, 'cdp-shot1.png'), 'wb').write(png1)
    log('wrote cdp-shot1.png', len(png1), 'bytes')

    log('startScreencast')
    ws.call('Page.startScreencast',
            {'format': 'jpeg', 'quality': 60, 'maxWidth': 800, 'maxHeight': 800},
            on_event, session)
    ws.pump(3, on_event)

    log('scroll via Input.dispatchMouseEvent mouseWheel')
    ws.call('Input.dispatchMouseEvent',
            {'type': 'mouseWheel', 'x': 200, 'y': 300, 'deltaX': 0, 'deltaY': 600},
            on_event, session)
    ws.pump(2.5, on_event)

    log('captureScreenshot #2')
    r = ws.call('Page.captureScreenshot', {'format': 'png'}, on_event, session)
    png2 = base64.b64decode(r['data'])
    open(os.path.join(OUT, 'cdp-shot2.png'), 'wb').write(png2)
    log('wrote cdp-shot2.png', len(png2), 'bytes')

    if first_frame[0]:
        open(os.path.join(OUT, 'cdp-frame1.jpg'), 'wb').write(first_frame[0])
        log('wrote cdp-frame1.jpg', len(first_frame[0]), 'bytes')

    log('RESULT screencastFrames=%d shot1=%d shot2=%d' %
        (frames[0], len(png1), len(png2)))
    log('DONE')

main()
