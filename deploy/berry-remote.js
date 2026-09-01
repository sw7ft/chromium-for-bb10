#!/usr/bin/env node
/*
 * berry-remote.js -- browser-accessible "VNC-like" remote view + control of a
 * Chromium content_shell running on the BB10/QNX device.
 *
 * How it works (no device root required):
 *   - Spawns content_shell with --remote-debugging-pipe (CDP over fds 3/4; no
 *     TCP debug port needed, which is the robust path on QNX).
 *   - Page.startScreencast streams live JPEG frames of the page; we relay them
 *     to connected browsers as an MJPEG stream (multipart/x-mixed-replace) so a
 *     plain <img> shows a live, auto-updating mirror.
 *   - Browser sends mouse / wheel / keyboard / navigation back over HTTP POST,
 *     which we inject via CDP Input.dispatch* / Page.navigate -- so you can
 *     actually drive the page from your desktop browser.
 *
 * This mirrors+controls the content_shell instance THIS script launches (a
 * separate Chromium from the installed .bar app, with its own profile). Point it
 * at web.whatsapp.com to use WhatsApp fully from a desktop browser.
 *
 * Why not a true whole-device VNC: capturing other apps / the composited display
 * (the root-only `snapshot` tool / privileged Screen capture) needs device root,
 * which we don't have over SSH (we are devuser). CDP screencast needs no
 * privilege and gives view + control of the browser engine, which is the part we
 * own and care about here.
 *
 * Run ON the device, from a dir containing content_shell + paks + libs:
 *   PORT=8080 URL=https://web.whatsapp.com/ node berry-remote.js
 */
'use strict';

var http = require('http');
var spawn = require('child_process').spawn;
var path = require('path');
var fs = require('fs');

var DIR = process.cwd();
var BIN = path.join(DIR, 'content_shell');
var PORT = parseInt(process.env.PORT || '8080', 10);
var HOST = process.env.HOST || '0.0.0.0';
var START_URL = process.env.URL || 'https://web.whatsapp.com/';
var RENDER = parseInt(process.env.RENDER || '720', 10);
var JPEG_QUALITY = parseInt(process.env.QUALITY || '60', 10);

function log() {
  process.stdout.write('[remote] ' +
    Array.prototype.slice.call(arguments).join(' ') + '\n');
}

// ---------------------------------------------------------------------------
// Launch content_shell with the WhatsApp-style flag set + the CDP pipe.
// ---------------------------------------------------------------------------
var env = Object.assign({}, process.env, {
  // content_shell gates DevTools/CDP behind this env on QNX (see
  // shell_browser_main_parts.cc: StartHttpHandler, which also starts the
  // --remote-debugging-pipe handler, only runs when QNX_DEVTOOLS is set).
  QNX_DEVTOOLS: '1',
  LD_LIBRARY_PATH: DIR + ':' + (process.env.LD_LIBRARY_PATH || ''),
  QNX_SCREEN_WIDTH: String(RENDER),
  QNX_SCREEN_HEIGHT: String(RENDER),
  QNX_SCREEN_OUTPUT_WIDTH: '1440',
  QNX_SCREEN_OUTPUT_HEIGHT: '1440',
  QNX_SCREEN_ROTATION: process.env.QNX_SCREEN_ROTATION || '90'
});
var ca = path.join(DIR, 'root_store.certs');
if (fs.existsSync(ca)) env.QNX_CA_BUNDLE = ca;

var args = [
  '--no-sandbox', '--no-zygote', '--single-process',
  '--disable-gpu', '--disable-gpu-compositing',
  '--ozone-platform=qnx_screen',
  '--disable-renderer-accessibility',
  '--disable-frame-rate-limit',
  '--ignore-certificate-errors',
  '--use-fake-ui-for-media-stream',
  '--disable-quic',
  // ServiceWorker LEFT ENABLED (WhatsApp app-shell cache); the rest are the
  // QNX-unstable networking/IPC features.
  '--disable-features=NetworkServiceDedicatedThread,MojoIpcz,Translate,' +
    'OptimizationHints,MediaRouter,PreconnectToSearch',
  '--force-device-scale-factor=1',
  '--remote-debugging-pipe', '--remote-allow-origins=*',
  'about:blank'
];

log('engine:', BIN);
log('url:', START_URL, 'render:', RENDER);

var child = spawn(BIN, args, {
  cwd: DIR, env: env, stdio: ['ignore', 'ignore', 'pipe', 'pipe', 'pipe']
});
child.on('error', function (e) { log('spawn error', e.message); process.exit(1); });
child.on('exit', function (c, s) {
  log('engine exited code=' + c + ' sig=' + s); process.exit(1);
});

var elog = fs.openSync(path.join(DIR, 'berry-remote-engine.log'), 'w');
child.stdio[2].on('data', function (d) { try { fs.writeSync(elog, d); } catch (e) {} });

// ---------------------------------------------------------------------------
// Minimal CDP-over-pipe client (NUL-delimited JSON on fds 3=write, 4=read).
// ---------------------------------------------------------------------------
var pipeWrite = child.stdio[3];
var pipeRead = child.stdio[4];
var nextId = 0, pending = {}, listeners = [], rbuf = Buffer.alloc(0);

pipeRead.on('data', function (chunk) {
  rbuf = Buffer.concat([rbuf, chunk]);
  var start = 0;
  for (var i = 0; i < rbuf.length; i++) {
    if (rbuf[i] === 0) {
      var s = rbuf.slice(start, i); start = i + 1;
      if (s.length) dispatch(s.toString('utf8'));
    }
  }
  rbuf = rbuf.slice(start);
});
function dispatch(text) {
  var msg; try { msg = JSON.parse(text); } catch (e) { return; }
  if (msg.id && pending[msg.id]) {
    var p = pending[msg.id]; delete pending[msg.id];
    if (msg.error) p.reject(new Error(JSON.stringify(msg.error)));
    else p.resolve(msg.result || {});
  } else if (msg.method) {
    for (var k = 0; k < listeners.length; k++) listeners[k](msg);
  }
}
function send(method, params, sid) {
  return new Promise(function (resolve, reject) {
    var id = ++nextId; pending[id] = { resolve: resolve, reject: reject };
    var e = { id: id, method: method, params: params || {} };
    if (sid) e.sessionId = sid;
    pipeWrite.write(JSON.stringify(e) + '\0');
    setTimeout(function () {
      if (pending[id]) { delete pending[id]; reject(new Error('timeout: ' + method)); }
    }, 15000);
  });
}
function sendRaw(o) { pipeWrite.write(JSON.stringify(o) + '\0'); }
function sleep(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }

// ---------------------------------------------------------------------------
// Screencast state + MJPEG fan-out.
// ---------------------------------------------------------------------------
var session = null, attachResolve = null;
var lastFrame = null;            // latest JPEG Buffer
var lastMeta = { deviceWidth: RENDER, deviceHeight: RENDER,
                 pageScaleFactor: 1, offsetTop: 0,
                 scrollOffsetX: 0, scrollOffsetY: 0 };
var mjpegClients = [];           // array of http res
var frameCount = 0;

listeners.push(function (msg) {
  if (msg.method === 'Target.attachedToTarget') {
    var ti = msg.params.targetInfo || {};
    if (ti.type === 'page') {
      session = msg.params.sessionId;
      if (attachResolve) { var r = attachResolve; attachResolve = null; r(); }
    }
  } else if (msg.method === 'Page.screencastFrame') {
    frameCount++;
    if (msg.params.metadata) lastMeta = msg.params.metadata;
    var buf = Buffer.from(msg.params.data, 'base64');
    lastFrame = buf;
    // Ack so the engine keeps sending frames.
    sendRaw({ id: 900000 + frameCount, method: 'Page.screencastFrameAck',
              params: { sessionId: msg.params.sessionId }, sessionId: session });
    pushFrameToClients(buf);
  }
});

function pushFrameToClients(buf) {
  for (var i = mjpegClients.length - 1; i >= 0; i--) {
    var res = mjpegClients[i];
    try {
      res.write('--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ' +
                buf.length + '\r\n\r\n');
      res.write(buf);
      res.write('\r\n');
    } catch (e) {
      mjpegClients.splice(i, 1);
    }
  }
}

function waitForSession() {
  return new Promise(function (resolve, reject) {
    if (session) return resolve();
    attachResolve = resolve;
    setTimeout(function () {
      if (attachResolve) { attachResolve = null; reject(new Error('no page session')); }
    }, 20000);
  });
}

function startScreencast() {
  return send('Page.startScreencast', {
    format: 'jpeg', quality: JPEG_QUALITY,
    maxWidth: RENDER, maxHeight: RENDER, everyNthFrame: 1
  }, session);
}

// ---------------------------------------------------------------------------
// Input injection helpers (coords arrive normalized 0..1 from the browser).
// ---------------------------------------------------------------------------
function cssX(fx) { return Math.round(fx * (lastMeta.deviceWidth || RENDER)); }
function cssY(fy) { return Math.round(fy * (lastMeta.deviceHeight || RENDER)); }

function mouse(type, fx, fy, buttons, button, clickCount) {
  return send('Input.dispatchMouseEvent', {
    type: type, x: cssX(fx), y: cssY(fy),
    button: button || (buttons ? 'left' : 'none'),
    buttons: buttons || 0,
    clickCount: clickCount || 0
  }, session);
}
function wheel(fx, fy, dx, dy) {
  return send('Input.dispatchMouseEvent', {
    type: 'mouseWheel', x: cssX(fx), y: cssY(fy),
    deltaX: dx || 0, deltaY: dy || 0
  }, session);
}
function keyEvent(ev) {
  // ev: {type:'keyDown'|'keyUp'|'char', text, key, code, keyCode}
  var p = { type: ev.type };
  if (ev.text !== undefined) p.text = ev.text;
  if (ev.key !== undefined) p.key = ev.key;
  if (ev.code !== undefined) p.code = ev.code;
  if (ev.keyCode !== undefined) {
    p.windowsVirtualKeyCode = ev.keyCode;
    p.nativeVirtualKeyCode = ev.keyCode;
  }
  return send('Input.dispatchKeyEvent', p, session);
}

// ---------------------------------------------------------------------------
// HTTP server: page + MJPEG stream + input/nav endpoints.
// ---------------------------------------------------------------------------
function readBody(req) {
  return new Promise(function (resolve) {
    var b = '';
    req.on('data', function (d) { b += d; if (b.length > 1e6) b = b.slice(0, 1e6); });
    req.on('end', function () { resolve(b); });
  });
}

var server = http.createServer(function (req, res) {
  var u = req.url.split('?')[0];

  if (u === '/' || u === '/index.html') {
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
    res.end(PAGE_HTML);
    return;
  }

  if (u === '/stream') {
    res.writeHead(200, {
      'Content-Type': 'multipart/x-mixed-replace; boundary=frame',
      'Cache-Control': 'no-cache, no-store, must-revalidate',
      'Pragma': 'no-cache', 'Connection': 'close'
    });
    mjpegClients.push(res);
    if (lastFrame) pushFrameToClients(lastFrame);
    req.on('close', function () {
      var i = mjpegClients.indexOf(res);
      if (i >= 0) mjpegClients.splice(i, 1);
    });
    return;
  }

  if (u === '/input' && req.method === 'POST') {
    readBody(req).then(function (b) {
      var ev; try { ev = JSON.parse(b); } catch (e) { ev = null; }
      if (ev && session) {
        try {
          if (ev.kind === 'down') mouse('mousePressed', ev.fx, ev.fy, 1, 'left', 1);
          else if (ev.kind === 'up') mouse('mouseReleased', ev.fx, ev.fy, 0, 'left', 1);
          else if (ev.kind === 'move') mouse('mouseMoved', ev.fx, ev.fy, ev.buttons ? 1 : 0);
          else if (ev.kind === 'wheel') wheel(ev.fx, ev.fy, ev.dx, ev.dy);
        } catch (e) {}
      }
      res.writeHead(204); res.end();
    });
    return;
  }

  if (u === '/key' && req.method === 'POST') {
    readBody(req).then(function (b) {
      var ev; try { ev = JSON.parse(b); } catch (e) { ev = null; }
      if (ev && session) { try { keyEvent(ev); } catch (e) {} }
      res.writeHead(204); res.end();
    });
    return;
  }

  if (u === '/nav' && req.method === 'POST') {
    readBody(req).then(function (b) {
      var o; try { o = JSON.parse(b); } catch (e) { o = null; }
      if (o && o.url && session) {
        sendRaw({ id: ++nextId, method: 'Page.navigate',
                  params: { url: o.url }, sessionId: session });
      }
      res.writeHead(204); res.end();
    });
    return;
  }

  // Single-frame endpoint for browsers without multipart/x-mixed-replace
  // support (old ES5-era engines): the viewer polls this on a timer.
  if (u === '/frame.jpg') {
    if (!lastFrame) { res.writeHead(503); res.end(); return; }
    res.writeHead(200, {
      'Content-Type': 'image/jpeg', 'Content-Length': lastFrame.length,
      'Cache-Control': 'no-cache, no-store, must-revalidate', 'Pragma': 'no-cache'
    });
    res.end(lastFrame);
    return;
  }

  if (u === '/status') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({
      session: !!session, frames: frameCount,
      clients: mjpegClients.length, meta: lastMeta
    }));
    return;
  }

  res.writeHead(404); res.end('not found');
});

// ---------------------------------------------------------------------------
// Browser UI. Lives in berry-viewer.html beside this script. It used to be a
// big inline string literal, but the experimental QNX ARM32 node build
// SIGBUSes while (background-)compiling this file with the full UI inlined --
// loading it as data at runtime sidesteps that and keeps the UI editable.
// ---------------------------------------------------------------------------
var PAGE_HTML;
try {
  PAGE_HTML = fs.readFileSync(path.join(__dirname, 'berry-viewer.html'), 'utf8')
      .split('%%START_URL%%').join(START_URL);
} catch (e) {
  PAGE_HTML = '<!doctype html><html><body style="background:#111;color:#ddd">' +
      '<p>berry-viewer.html missing next to berry-remote.js</p>' +
      '<img src="/stream"></body></html>';
}

// ---------------------------------------------------------------------------
// Boot: attach to the page target, navigate, start screencast, listen.
// ---------------------------------------------------------------------------
function boot() {
  send('Target.setAutoAttach',
       { autoAttach: true, waitForDebuggerOnStart: false, flatten: true })
    .then(function () { log('autoAttach ok; waiting for page session'); return waitForSession(); })
    .then(function () {
      log('page session', session, '-> navigate', START_URL);
      sendRaw({ id: ++nextId, method: 'Page.enable', params: {}, sessionId: session });
      sendRaw({ id: ++nextId, method: 'Page.navigate', params: { url: START_URL }, sessionId: session });
      return sleep(2500);
    })
    .then(function () { log('startScreencast'); return startScreencast(); })
    .then(function () {
      server.listen(PORT, HOST, function () {
        log('LISTENING http://' + HOST + ':' + PORT + '  (open in a browser)');
      });
    })
    .catch(function (e) {
      log('BOOT FAIL', e.message);
      try { child.kill('SIGKILL'); } catch (e2) {}
      process.exit(3);
    });
}
setTimeout(boot, 1500);

// Periodically re-issue screencast if frames stop (e.g. after navigation).
setInterval(function () {
  if (session) { try { startScreencast(); } catch (e) {} }
}, 15000);
