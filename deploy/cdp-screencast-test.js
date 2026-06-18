#!/usr/bin/env node
/*
 * On-device screencast test: drive content_shell on qnx_screen over CDP, start
 * Page.startScreencast, and collect streamed frames. Dispatches a click that
 * changes the page so we can confirm the stream reflects live updates (frame
 * count > 1 and a post-click frame differs). Saves first + last frame.
 * Runs ON the device. Output to OUT (default /tmp).
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
  '<div id="t" style="height:200px;background:#cc0000;color:#fff;',
  'font-size:56px;text-align:center;line-height:200px">FRAME 0</div>',
  '<div style="height:1200px;background:#0000ff"></div>',
  '<script>var n=0;document.getElementById("t").addEventListener("click",',
  'function(){n++;this.style.background="#00aa00";this.textContent="FRAME "+n;});',
  'setInterval(function(){var t=document.getElementById("t");',
  't.style.borderBottom=(Date.now()%1000<500?"20px solid #fff":"20px solid #000");},250);',
  '<\/script></body>'
].join('');
var URL = 'data:text/html,' + encodeURIComponent(PAGE);

function log() { process.stdout.write('[sc] ' + Array.prototype.slice.call(arguments).join(' ') + '\n'); }

var args = ['--no-sandbox', '--no-zygote', '--single-process',
  '--disable-gpu', '--disable-gpu-compositing', '--ozone-platform=qnx_screen',
  '--remote-debugging-pipe', '--remote-allow-origins=*', 'about:blank'];

var env = Object.assign({}, process.env, { LD_LIBRARY_PATH: DIR + ':' + (process.env.LD_LIBRARY_PATH || '') });
var child = spawn(BIN, args, { cwd: DIR, env: env, stdio: ['ignore', 'ignore', 'pipe', 'pipe', 'pipe'] });
child.on('exit', function (c, s) { log('engine exited code=' + c + ' sig=' + s); });
var elog = process.env.ENGINE_LOG ? fs.openSync(process.env.ENGINE_LOG, 'w') : null;
child.stdio[2].on('data', function (d) { if (elog) { try { fs.writeSync(elog, d); } catch (e) {} } });

var pipeWrite = child.stdio[3], pipeRead = child.stdio[4];
var nextId = 0, pending = {}, listeners = [], rbuf = Buffer.alloc(0);
pipeRead.on('data', function (chunk) {
  rbuf = Buffer.concat([rbuf, chunk]); var start = 0;
  for (var i = 0; i < rbuf.length; i++) { if (rbuf[i] === 0) { var s = rbuf.slice(start, i); start = i + 1; if (s.length) dispatch(s.toString('utf8')); } }
  rbuf = rbuf.slice(start);
});
function dispatch(text) {
  var msg; try { msg = JSON.parse(text); } catch (e) { return; }
  if (msg.id && pending[msg.id]) { var p = pending[msg.id]; delete pending[msg.id]; if (msg.error) p.reject(new Error(JSON.stringify(msg.error))); else p.resolve(msg.result || {}); }
  else if (msg.method) { for (var k = 0; k < listeners.length; k++) listeners[k](msg); }
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
var frames = 0, firstFrame = null, lastFrame = null, framesAfterClick = 0, clicked = false;
listeners.push(function (msg) {
  if (msg.method === 'Target.attachedToTarget') {
    var ti = msg.params.targetInfo || {};
    if (ti.type === 'page') { session = msg.params.sessionId; if (attachResolve) { var r = attachResolve; attachResolve = null; r(); } }
  }
  if (msg.method === 'Page.screencastFrame') {
    frames++; if (clicked) framesAfterClick++;
    var buf = Buffer.from(msg.params.data, 'base64');
    if (!firstFrame) firstFrame = buf;
    lastFrame = buf;
    sendRaw({ id: 900000 + frames, method: 'Page.screencastFrameAck', params: { sessionId: msg.params.sessionId }, sessionId: session });
  }
});
function waitForSession() {
  return new Promise(function (resolve, reject) {
    if (session) return resolve();
    attachResolve = resolve;
    setTimeout(function () { if (attachResolve) { attachResolve = null; reject(new Error('no page session')); } }, 15000);
  });
}

function run() {
  send('Target.setAutoAttach', { autoAttach: true, waitForDebuggerOnStart: false, flatten: true })
    .then(function () { return waitForSession(); })
    .then(function () { log('session', session, 'navigate'); sendRaw({ id: ++nextId, method: 'Page.navigate', params: { url: URL }, sessionId: session }); return sleep(6000); })
    .then(function () { log('startScreencast'); return send('Page.startScreencast', { format: 'jpeg', quality: 70, maxWidth: 800, maxHeight: 800, everyNthFrame: 1 }, session); })
    .then(function () { return sleep(3000); })
    .then(function () { log('frames before click:', frames, '-> CLICK'); clicked = true; return send('Input.dispatchMouseEvent', { type: 'mousePressed', x: 400, y: 100, button: 'left', clickCount: 1, buttons: 1 }, session).then(function () { return send('Input.dispatchMouseEvent', { type: 'mouseReleased', x: 400, y: 100, button: 'left', clickCount: 1, buttons: 0 }, session); }); })
    .then(function () { return sleep(3000); })
    .then(function () {
      if (firstFrame) { fs.writeFileSync(path.join(OUT, 'sc-first.jpg'), firstFrame); log('wrote sc-first.jpg', firstFrame.length); }
      if (lastFrame) { fs.writeFileSync(path.join(OUT, 'sc-last.jpg'), lastFrame); log('wrote sc-last.jpg', lastFrame.length); }
      log('RESULT totalFrames=' + frames + ' framesAfterClick=' + framesAfterClick);
      try { child.kill('SIGKILL'); } catch (e) {} process.exit(0);
    })
    .catch(function (e) { log('FAIL', e.message); try { child.kill('SIGKILL'); } catch (e2) {} process.exit(3); });
}
setTimeout(run, 1500);
