#!/usr/bin/env node
/*
 * On-device interactivity test: drive content_shell on the real qnx_screen
 * display over CDP (--remote-debugging-pipe). Loads an interactive page, then
 * injects a CDP mouse click and a scroll, capturing before/after screenshots.
 * A click handler turns the tap target green ("CLICKED OK"), so a visible color
 * change in shot2 proves input -> renderer -> JS -> repaint works end to end.
 *
 * Runs ON the device. Screenshots written to OUT (default /tmp).
 */
'use strict';
var spawn = require('child_process').spawn;
var fs = require('fs');
var path = require('path');

var OUT = process.argv[2] || '/tmp';
var DIR = process.cwd();
var BIN = path.join(DIR, 'content_shell');

var PAGE = [
  '<body style="margin:0;font-family:sans-serif">',
  '<div id="t" style="height:160px;background:#cc0000;color:#fff;',
  'font-size:56px;text-align:center;line-height:160px">TAP TARGET</div>',
  '<div style="height:300px;background:#ffff00;font-size:40px">TOP-YELLOW</div>',
  '<div style="height:2000px;background:#0000ff;color:#fff;font-size:40px">',
  'SCROLL-BLUE</div>',
  '<script>document.getElementById("t").addEventListener("click",function(){',
  'this.style.background="#00aa00";this.textContent="CLICKED OK";});<\/script>',
  '</body>'
].join('');
var URL = 'data:text/html,' + encodeURIComponent(PAGE);

function log() {
  process.stdout.write('[in] ' + Array.prototype.slice.call(arguments).join(' ') + '\n');
}

var args = [
  '--no-sandbox', '--no-zygote', '--single-process',
  '--disable-gpu', '--disable-gpu-compositing',
  '--ozone-platform=qnx_screen',
  '--remote-debugging-pipe', '--remote-allow-origins=*',
  'about:blank'
];
log('args:', args.join(' '));

var env = Object.assign({}, process.env, {
  LD_LIBRARY_PATH: DIR + ':' + (process.env.LD_LIBRARY_PATH || '')
});

var child = spawn(BIN, args, {
  cwd: DIR, env: env, stdio: ['ignore', 'ignore', 'pipe', 'pipe', 'pipe']
});
child.on('error', function (e) { log('spawn error', e.message); process.exit(1); });
child.on('exit', function (c, s) { log('engine exited code=' + c + ' sig=' + s); });

var elog = process.env.ENGINE_LOG ? fs.openSync(process.env.ENGINE_LOG, 'w') : null;
child.stdio[2].on('data', function (d) { if (elog) { try { fs.writeSync(elog, d); } catch (e) {} } });

var pipeWrite = child.stdio[3];
var pipeRead = child.stdio[4];
var nextId = 0, pending = {}, listeners = [], rbuf = Buffer.alloc(0);

pipeRead.on('data', function (chunk) {
  rbuf = Buffer.concat([rbuf, chunk]);
  var start = 0;
  for (var i = 0; i < rbuf.length; i++) {
    if (rbuf[i] === 0) { var s = rbuf.slice(start, i); start = i + 1; if (s.length) dispatch(s.toString('utf8')); }
  }
  rbuf = rbuf.slice(start);
});

function dispatch(text) {
  var msg; try { msg = JSON.parse(text); } catch (e) { return; }
  if (msg.id && pending[msg.id]) {
    var p = pending[msg.id]; delete pending[msg.id];
    if (msg.error) p.reject(new Error(JSON.stringify(msg.error))); else p.resolve(msg.result || {});
  } else if (msg.method) { for (var k = 0; k < listeners.length; k++) listeners[k](msg); }
}
function send(method, params, sid) {
  return new Promise(function (resolve, reject) {
    var id = ++nextId; pending[id] = { resolve: resolve, reject: reject };
    var e = { id: id, method: method, params: params || {} }; if (sid) e.sessionId = sid;
    pipeWrite.write(JSON.stringify(e) + '\0');
    setTimeout(function () { if (pending[id]) { delete pending[id]; reject(new Error('timeout: ' + method)); } }, 15000);
  });
}
function sendRaw(o) { pipeWrite.write(JSON.stringify(o) + '\0'); }
function sleep(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }

var session = null, attachResolve = null;
listeners.push(function (msg) {
  if (msg.method === 'Target.attachedToTarget') {
    var ti = msg.params.targetInfo || {};
    if (ti.type === 'page') { session = msg.params.sessionId; if (attachResolve) { var r = attachResolve; attachResolve = null; r(); } }
  }
});
function waitForSession() {
  return new Promise(function (resolve, reject) {
    if (session) return resolve();
    attachResolve = resolve;
    setTimeout(function () { if (attachResolve) { attachResolve = null; reject(new Error('no page session')); } }, 15000);
  });
}
function shot(name) {
  return send('Page.captureScreenshot', { format: 'png' }, session).then(function (r) {
    var b = Buffer.from(r.data, 'base64'); fs.writeFileSync(path.join(OUT, name), b); log('wrote', name, b.length, 'bytes');
  });
}
function click(x, y) {
  return send('Input.dispatchMouseEvent', { type: 'mousePressed', x: x, y: y, button: 'left', clickCount: 1, buttons: 1 }, session)
    .then(function () { return send('Input.dispatchMouseEvent', { type: 'mouseReleased', x: x, y: y, button: 'left', clickCount: 1, buttons: 0 }, session); });
}

function run() {
  send('Target.setAutoAttach', { autoAttach: true, waitForDebuggerOnStart: false, flatten: true })
    .then(function () { log('autoAttach ok; waiting session'); return waitForSession(); })
    .then(function () { log('session', session, 'navigate'); sendRaw({ id: ++nextId, method: 'Page.navigate', params: { url: URL }, sessionId: session }); return sleep(7000); })
    .then(function () { return shot('in-shot1.png'); })
    .then(function () { log('CLICK tap target @ (400,80)'); return click(400, 80); })
    .then(function () { return sleep(2500); })
    .then(function () { return shot('in-shot2.png'); })
    .then(function () { log('SCROLL down 800'); return send('Input.dispatchMouseEvent', { type: 'mouseWheel', x: 400, y: 400, deltaX: 0, deltaY: 800 }, session); })
    .then(function () { return sleep(2500); })
    .then(function () { return shot('in-shot3.png'); })
    .then(function () { log('DONE'); try { child.kill('SIGKILL'); } catch (e) {} process.exit(0); })
    .catch(function (e) { log('FAIL', e.message); try { child.kill('SIGKILL'); } catch (e2) {} process.exit(3); });
}
setTimeout(run, 1500);
