#!/usr/bin/env node
/*
 * logship-server.js -- tiny dependency-free receiver for Berry Browser boot
 * logs. Each device POSTs the tail of berry-kbd.log on every browser launch
 * (see launcher.c berry_ship_log); this stores them per-device and serves a
 * simple browser UI to read them.
 *
 * Run (any VPS / always-on box with node >= 10):
 *   PORT=8787 TOKEN=mysecret node logship-server.js
 *
 * Device setup (one marker file; also settable in Settings > Developer):
 *   echo "myhost.example.com:8787/ingest?t=mysecret" \
 *     > /accounts/1000/shared/misc/berry-logship-url
 *
 * Endpoints:
 *   POST /ingest?device=<id>&build=<n>[&t=<token>]  append log chunk
 *   GET  /?t=<token>                                device list (HTML)
 *   GET  /log/<device>?t=<token>                    raw log (text/plain)
 *
 * TOKEN is optional but recommended; when set, all requests must carry
 * ?t=<token>. Logs rotate at 25 MB per device (old file moved to .1).
 */
'use strict';

var http = require('http');
var fs = require('fs');
var path = require('path');
var url = require('url');

var PORT = parseInt(process.env.PORT || '8787', 10);
var HOST = process.env.HOST || '0.0.0.0';
var TOKEN = process.env.TOKEN || '';
var DIR = process.env.LOG_DIR || path.join(__dirname, 'device-logs');
var MAX_BYTES = 25 * 1024 * 1024;      // rotate threshold per device
var MAX_CHUNK = 512 * 1024;            // reject bodies larger than this

if (!fs.existsSync(DIR)) fs.mkdirSync(DIR, { recursive: true });

function safeName(s) {
  return String(s || 'unknown').replace(/[^a-zA-Z0-9._-]/g, '_').slice(0, 64);
}

function logPath(device) {
  return path.join(DIR, safeName(device) + '.log');
}

function rotateIfNeeded(file) {
  try {
    var st = fs.statSync(file);
    if (st.size > MAX_BYTES) fs.renameSync(file, file + '.1');
  } catch (e) { /* no file yet */ }
}

function deny(res, code, msg) {
  res.writeHead(code, { 'Content-Type': 'text/plain' });
  res.end(msg + '\n');
}

http.createServer(function (req, res) {
  var u = url.parse(req.url, true);
  if (TOKEN && u.query.t !== TOKEN) return deny(res, 403, 'bad token');

  // ---- ingest ----
  if (req.method === 'POST' && u.pathname === '/ingest') {
    var device = safeName(u.query.device);
    var build = String(u.query.build || '?').replace(/[^0-9]/g, '');
    var chunks = [];
    var size = 0;
    req.on('data', function (c) {
      size += c.length;
      if (size > MAX_CHUNK) { req.destroy(); return; }
      chunks.push(c);
    });
    req.on('end', function () {
      var file = logPath(device);
      rotateIfNeeded(file);
      var sep = '\n===== logship ' + new Date().toISOString() +
                ' device=' + device + ' build=' + build +
                ' bytes=' + size + ' =====\n';
      fs.appendFile(file, sep + Buffer.concat(chunks).toString('utf8'),
                    function () {});
      res.writeHead(200, { 'Content-Type': 'text/plain' });
      res.end('ok\n');
      console.log('[logship] ' + device + ' build=' + build +
                  ' +' + size + 'B');
    });
    return;
  }

  // ---- raw log ----
  if (req.method === 'GET' && u.pathname.indexOf('/log/') === 0) {
    var file2 = logPath(u.pathname.slice(5));
    if (!fs.existsSync(file2)) return deny(res, 404, 'no such device');
    res.writeHead(200, { 'Content-Type': 'text/plain; charset=utf-8' });
    fs.createReadStream(file2).pipe(res);
    return;
  }

  // ---- device list ----
  if (req.method === 'GET' && (u.pathname === '/' || u.pathname === '')) {
    var rows = fs.readdirSync(DIR).filter(function (f) {
      return f.slice(-4) === '.log';
    }).map(function (f) {
      var st = fs.statSync(path.join(DIR, f));
      var dev = f.slice(0, -4);
      var q = TOKEN ? '?t=' + encodeURIComponent(TOKEN) : '';
      return '<tr><td><a href="/log/' + dev + q + '">' + dev + '</a></td>' +
             '<td>' + (st.size / 1024).toFixed(0) + ' KB</td>' +
             '<td>' + st.mtime.toISOString() + '</td></tr>';
    }).join('');
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
    res.end('<!doctype html><title>Berry logs</title>' +
            '<style>body{font-family:sans-serif;background:#1a0012;' +
            'color:#f3e6ef;padding:24px}a{color:#e95420}' +
            'table{border-collapse:collapse}td{padding:6px 16px;' +
            'border-bottom:1px solid #444}</style>' +
            '<h2>Berry Browser device logs</h2>' +
            '<table><tr><td><b>device</b></td><td><b>size</b></td>' +
            '<td><b>last upload</b></td></tr>' + rows + '</table>');
    return;
  }

  deny(res, 404, 'not found');
}).listen(PORT, HOST, function () {
  console.log('[logship] listening on ' + HOST + ':' + PORT +
              ' dir=' + DIR + (TOKEN ? ' (token required)' : ' (NO TOKEN)'));
});
