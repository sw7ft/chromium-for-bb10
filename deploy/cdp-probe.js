#!/usr/bin/env node
/*
 * Dependency-free CDP probe for validating headless content_shell rendering.
 * Talks raw WebSocket (no npm deps) to a remote-debugging endpoint.
 *
 * Usage: node cdp-probe.js [host] [port] [url] [outDir]
 *   defaults: 127.0.0.1 9222 https://example.com /tmp
 *
 * Steps: discover page target -> navigate -> captureScreenshot ->
 *        startScreencast (collect frames) -> scroll via Input -> screenshot.
 * Writes PNGs to outDir and prints a summary so we can eyeball the result.
 */
'use strict';
var http = require('http');
var crypto = require('crypto');
var fs = require('fs');
var path = require('path');

var HOST = process.argv[2] || '127.0.0.1';
var PORT = parseInt(process.argv[3] || '9222', 10);
var URL = process.argv[4] || 'https://example.com';
var OUT = process.argv[5] || '/tmp';

function log() {
  var a = Array.prototype.slice.call(arguments);
  process.stdout.write('[cdp] ' + a.join(' ') + '\n');
}
function fail(msg) { log('FAIL:', msg); process.exit(1); }

function httpGetJson(pathname, cb) {
  http.get({ host: HOST, port: PORT, path: pathname }, function (res) {
    var d = [];
    res.on('data', function (c) { d.push(c); });
    res.on('end', function () {
      try { cb(null, JSON.parse(Buffer.concat(d).toString('utf8'))); }
      catch (e) { cb(e); }
    });
  }).on('error', cb);
}

// ---- Minimal WebSocket client ----
function WS(wsUrl, onOpen, onMessage, onErr) {
  var m = /^ws:\/\/([^:/]+):(\d+)(\/.*)$/.exec(wsUrl);
  if (!m) return onErr(new Error('bad ws url ' + wsUrl));
  var key = crypto.randomBytes(16).toString('base64');
  var req = http.request({
    host: m[1], port: parseInt(m[2], 10), path: m[3],
    headers: {
      Connection: 'Upgrade', Upgrade: 'websocket',
      'Sec-WebSocket-Key': key, 'Sec-WebSocket-Version': '13'
    }
  });
  var self = this;
  req.on('upgrade', function (res, socket) {
    self.sock = socket;
    var buf = Buffer.alloc(0);
    socket.on('data', function (chunk) {
      buf = Buffer.concat([buf, chunk]);
      for (;;) {
        if (buf.length < 2) return;
        var b0 = buf[0], b1 = buf[1];
        var op = b0 & 0x0f;
        var len = b1 & 0x7f;
        var off = 2;
        if (len === 126) { if (buf.length < 4) return; len = buf.readUInt16BE(2); off = 4; }
        else if (len === 127) { if (buf.length < 10) return; len = Number(buf.readBigUInt64BE(2)); off = 10; }
        if (buf.length < off + len) return;
        var payload = buf.slice(off, off + len);
        buf = buf.slice(off + len);
        if (op === 0x8) { socket.end(); return; }
        if (op === 0x1 || op === 0x2 || op === 0x0) onMessage(payload.toString('utf8'));
      }
    });
    socket.on('error', onErr);
    onOpen();
  });
  req.on('error', onErr);
  req.end();
}
WS.prototype.send = function (str) {
  var payload = Buffer.from(str, 'utf8');
  var len = payload.length;
  var header;
  var mask = crypto.randomBytes(4);
  if (len < 126) { header = Buffer.alloc(2); header[1] = 0x80 | len; }
  else if (len < 65536) { header = Buffer.alloc(4); header[1] = 0x80 | 126; header.writeUInt16BE(len, 2); }
  else { header = Buffer.alloc(10); header[1] = 0x80 | 127; header.writeBigUInt64BE(BigInt(len), 2); }
  header[0] = 0x81; // FIN + text
  var masked = Buffer.alloc(len);
  for (var i = 0; i < len; i++) masked[i] = payload[i] ^ mask[i & 3];
  this.sock.write(Buffer.concat([header, mask, masked]));
};

// ---- CDP session ----
function main(wsUrl) {
  var ws = null;
  var id = 0;
  var pending = {};
  var frames = 0;
  var firstFrameBytes = 0;

  function send(method, params) {
    return new Promise(function (resolve, reject) {
      var mid = ++id;
      pending[mid] = { resolve: resolve, reject: reject };
      ws.send(JSON.stringify({ id: mid, method: method, params: params || {} }));
    });
  }

  ws = new WS(wsUrl, function () { run(); }, function (msg) {
    var obj;
    try { obj = JSON.parse(msg); } catch (e) { return; }
    if (obj.id && pending[obj.id]) {
      var p = pending[obj.id]; delete pending[obj.id];
      if (obj.error) p.reject(new Error(JSON.stringify(obj.error)));
      else p.resolve(obj.result);
    } else if (obj.method === 'Page.screencastFrame') {
      frames++;
      var data = obj.params.data;
      if (frames === 1) firstFrameBytes = Buffer.from(data, 'base64').length;
      // ack so the stream keeps flowing
      ws.send(JSON.stringify({ id: ++id, method: 'Page.screencastFrameAck',
        params: { sessionId: obj.params.sessionId } }));
    }
  }, function (e) { fail('ws error ' + e.message); });

  function sleep(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }

  function run() {
    var loaded = false;
    Promise.resolve()
      .then(function () { return send('Page.enable'); })
      .then(function () { return send('Runtime.enable'); })
      .then(function () {
        log('navigate ->', URL);
        return send('Page.navigate', { url: URL });
      })
      .then(function () { return sleep(8000); }) // give it time to load+paint
      .then(function () {
        log('captureScreenshot #1');
        return send('Page.captureScreenshot', { format: 'png' });
      })
      .then(function (res) {
        var f = path.join(OUT, 'cdp-shot1.png');
        fs.writeFileSync(f, Buffer.from(res.data, 'base64'));
        log('wrote', f, Buffer.from(res.data, 'base64').length, 'bytes');
      })
      .then(function () {
        log('startScreencast');
        return send('Page.startScreencast', { format: 'jpeg', quality: 60, maxWidth: 800, maxHeight: 800 });
      })
      .then(function () { return sleep(3000); })
      .then(function () {
        log('scroll via Input.dispatchMouseEvent (wheel)');
        return send('Input.dispatchMouseEvent', { type: 'mouseWheel', x: 200, y: 300, deltaX: 0, deltaY: 600 });
      })
      .then(function () { return sleep(2500); })
      .then(function () {
        log('captureScreenshot #2 (after scroll)');
        return send('Page.captureScreenshot', { format: 'png' });
      })
      .then(function (res) {
        var f = path.join(OUT, 'cdp-shot2.png');
        fs.writeFileSync(f, Buffer.from(res.data, 'base64'));
        log('wrote', f, Buffer.from(res.data, 'base64').length, 'bytes');
      })
      .then(function () {
        log('RESULT screencastFrames=' + frames + ' firstFrameBytes=' + firstFrameBytes);
        log('DONE');
        process.exit(0);
      })
      .catch(function (e) { fail(e.message); });
  }
}

httpGetJson('/json', function (err, list) {
  if (err) return fail('GET /json: ' + err.message);
  var page = (list || []).filter(function (t) { return t.type === 'page'; })[0] || list[0];
  if (!page || !page.webSocketDebuggerUrl) return fail('no page target with ws url; targets=' + JSON.stringify(list));
  log('target:', page.url, page.webSocketDebuggerUrl);
  main(page.webSocketDebuggerUrl);
});
