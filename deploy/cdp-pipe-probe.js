#!/usr/bin/env node
/*
 * Phase 1 go/no-go probe: drive on-device headless content_shell over CDP using
 * --remote-debugging-pipe (fd3 in, fd4 out; \0-delimited JSON). No sockets, no
 * HTTP server (Chromium's net::HttpServer hangs on QNX). Runs ON the device.
 *
 * Usage: node cdp-pipe-probe.js [url] [outDir]
 *   defaults: https://example.com /tmp
 *
 * Validates: navigate -> captureScreenshot -> startScreencast frames ->
 *            Input scroll -> captureScreenshot. Writes PNG/JPEG, prints sizes.
 */
'use strict';
var spawn = require('child_process').spawn;
var fs = require('fs');
var path = require('path');

var URL = process.argv[2] || 'https://example.com';
var OUT = process.argv[3] || '/tmp';
var DIR = process.cwd();
var BIN = path.join(DIR, 'content_shell');

function log() {
  process.stdout.write('[cdp] ' + Array.prototype.slice.call(arguments).join(' ') + '\n');
}
function fail(m) { log('FAIL:', m); process.exit(1); }

var args = [
  '--no-sandbox', '--no-zygote',
  '--disable-gpu', '--disable-gpu-compositing',
  '--ozone-platform=headless', '--headless',
  '--remote-debugging-pipe',
  '--remote-allow-origins=*'
];
if (!process.env.NOSINGLE) args.splice(2, 0, '--single-process');
if (process.env.EXTRA) {
  process.env.EXTRA.split(/\s+/).filter(Boolean).forEach(function (f) { args.push(f); });
}
args.push('about:blank');
log('args:', args.join(' '));

var env = Object.assign({}, process.env, {
  LD_LIBRARY_PATH: DIR + ':' + (process.env.LD_LIBRARY_PATH || ''),
  SSL_CERT_FILE: path.join(DIR, 'cacert.pem')
});

log('spawn', BIN);
var child = spawn(BIN, args, {
  cwd: DIR, env: env,
  stdio: ['ignore', 'ignore', 'pipe', 'pipe', 'pipe']
});
child.on('error', function (e) { fail('spawn error ' + e.message); });
child.on('exit', function (code, sig) { log('engine exited code=' + code + ' sig=' + sig); });

// capture engine stderr (chromium logs) for diagnostics
var elog = process.env.ENGINE_LOG ? fs.openSync(process.env.ENGINE_LOG, 'w') : null;
child.stdio[2].on('data', function (d) {
  if (elog) { try { fs.writeSync(elog, d); } catch (e) {} }
  var s = d.toString('utf8');
  if (/ERROR|FATAL|DevTools|Cannot|listening/i.test(s)) process.stderr.write('[engine] ' + s);
});

var pipeWrite = child.stdio[3]; // parent -> child fd3 (engine reads)
var pipeRead = child.stdio[4];  // child fd4 -> parent (engine writes)

var nextId = 0;
var pending = {};
var listeners = [];
var rbuf = Buffer.alloc(0);

pipeRead.on('data', function (chunk) {
  rbuf = Buffer.concat([rbuf, chunk]);
  var start = 0;
  for (var i = 0; i < rbuf.length; i++) {
    if (rbuf[i] === 0) {
      var slice = rbuf.slice(start, i);
      start = i + 1;
      if (slice.length) dispatch(slice.toString('utf8'));
    }
  }
  rbuf = rbuf.slice(start);
});
pipeRead.on('error', function (e) { log('pipeRead error', e.message); });

function dispatch(text) {
  var msg;
  try { msg = JSON.parse(text); } catch (e) { return; }
  if (process.env.DIAG && msg.method !== 'Page.screencastFrame') {
    log('RX', text.length > 200 ? text.slice(0, 200) + '...' : text);
  }
  if (msg.id && pending[msg.id]) {
    var p = pending[msg.id]; delete pending[msg.id];
    if (msg.error) p.reject(new Error(JSON.stringify(msg.error)));
    else p.resolve(msg.result || {});
  } else if (msg.method) {
    for (var k = 0; k < listeners.length; k++) listeners[k](msg);
  }
}

function send(method, params, sessionId) {
  return new Promise(function (resolve, reject) {
    var id = ++nextId;
    pending[id] = { resolve: resolve, reject: reject };
    var env2 = { id: id, method: method, params: params || {} };
    if (sessionId) env2.sessionId = sessionId;
    pipeWrite.write(JSON.stringify(env2) + '\0');
    setTimeout(function () {
      if (pending[id]) { delete pending[id]; reject(new Error('timeout: ' + method)); }
    }, parseInt(process.env.CALL_TIMEOUT || '15000', 10));
  });
}
function sendRaw(obj) { pipeWrite.write(JSON.stringify(obj) + '\0'); }
function sleep(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }

var frames = 0, firstFrame = null, session = null;
var pageSession = null;
var attachResolve = null;
listeners.push(function (msg) {
  if (msg.method === 'Target.attachedToTarget') {
    var ti = msg.params.targetInfo || {};
    log('attachedToTarget type=' + ti.type + ' session=' + msg.params.sessionId);
    if (ti.type === 'page') {
      pageSession = msg.params.sessionId;
      if (attachResolve) { var r = attachResolve; attachResolve = null; r(pageSession); }
    }
  }
  if (msg.method === 'Page.screencastFrame') {
    frames++;
    if (!firstFrame) firstFrame = Buffer.from(msg.params.data, 'base64');
    sendRaw({ id: 900000 + frames, method: 'Page.screencastFrameAck',
              params: { sessionId: msg.params.sessionId }, sessionId: session });
  }
});

function waitForPageSession() {
  return new Promise(function (resolve, reject) {
    if (pageSession) return resolve(pageSession);
    attachResolve = resolve;
    setTimeout(function () { if (attachResolve) { attachResolve = null; reject(new Error('no page attachedToTarget')); } }, parseInt(process.env.ATTACH_TIMEOUT || '15000', 10));
  });
}

function navigateBestEffort() {
  // Page.navigate is browser-side-initiated but its CDP ack can be gated on the
  // (stalled) renderer agent. Fire it and don't block on the response.
  log('navigate (best-effort) ->', URL);
  sendRaw({ id: ++nextId, method: 'Page.navigate', params: { url: URL }, sessionId: session });
  return Promise.resolve();
}

function run() {
  var png1, png2;
  if (process.env.NOATTACH) {
    log('NOATTACH: pipe open, NOT sending setAutoAttach; waiting 15s then Browser.getVersion');
    sleep(15000)
      .then(function () { log('NOATTACH: sending Browser.getVersion (no auto-attach was ever sent)'); return send('Browser.getVersion', {}); })
      .then(function (r) { log('NOATTACH GETVERSION OK', JSON.stringify(r).slice(0, 100)); log('BROWSER-HEALTHY-WITHOUT-ATTACH'); try { child.kill('SIGKILL'); } catch (e) {} process.exit(0); })
      .catch(function (e) { log('NOATTACH GETVERSION FAIL', e.message); try { child.kill('SIGKILL'); } catch (e2) {} process.exit(3); });
    return;
  }
  // Auto-attach to all targets (flatten) -> we receive Target.attachedToTarget
  // with a sessionId for the page without an explicit attachToTarget round-trip.
  // NOTE: We deliberately skip Page.enable/Runtime.enable: their renderer-side
  // half stalls on QNX. captureScreenshot/startScreencast/Input.* are handled in
  // the browser process and don't require the renderer DevTools agent.
  send('Target.setAutoAttach', { autoAttach: true, waitForDebuggerOnStart: false, flatten: true })
    .then(function () { log('setAutoAttach ok; waiting for page session'); return waitForPageSession(); })
    .then(function (sid) {
      session = sid;
      log('attached session', session);
      if (process.env.MODE === 'bcmd') {
        log('MODE=bcmd: browser-side Browser.getVersion + Target.getTargets (no renderer)');
        return send('Browser.getVersion', {})
          .then(function (r) { log('GETVERSION OK', JSON.stringify(r).slice(0, 120)); return send('Target.getTargets', {}); })
          .then(function (r) { log('GETTARGETS OK n=' + ((r.targetInfos || []).length)); log('BROWSER-SIDE OK'); try { child.kill('SIGKILL'); } catch (e) {} process.exit(0); })
          .catch(function (e) { log('BROWSER-SIDE FAIL', e.message); try { child.kill('SIGKILL'); } catch (e2) {} process.exit(3); });
      }
      if (process.env.MODE === 'eval') {
        var pre = Promise.resolve();
        if (process.env.TRACE_ON) {
          pre = sleep(300).then(function () {
            log('TRACE_ON: sending SIGUSR1 to pid ' + child.pid + ' to enable QNX trace');
            try { process.kill(child.pid, 'SIGUSR1'); } catch (e) { log('SIGUSR1 err', e.message); }
            return sleep(400);
          });
        }
        return pre.then(function () {
          log('MODE=eval: Runtime.evaluate 1+1 (renderer main thread test)');
          return send('Runtime.evaluate', { expression: '1+1', returnByValue: true }, session)
            .then(function (r) { log('EVAL RESULT', JSON.stringify(r)); try { child.kill('SIGKILL'); } catch (e) {} process.exit(0); });
        });
      }
      if (process.env.MODE === 'pdf') {
        log('MODE=pdf: Page.printToPDF (compositor-free paint test)');
        return send('Page.printToPDF', { printBackground: true }, session)
          .then(function (r) { var b = Buffer.from(r.data, 'base64'); fs.writeFileSync(path.join(OUT, 'cdp-page.pdf'), b); log('PDF RESULT bytes=' + b.length); try { child.kill('SIGKILL'); } catch (e) {} process.exit(0); });
      }
      if (process.env.MODE === 'shot') {
        var navp = process.env.NONAV ? Promise.resolve() : navigateBestEffort();
        return navp
          .then(function () { return sleep(parseInt(process.env.SHOT_WAIT || '6000', 10)); })
          .then(function () { log('MODE=shot: Page.captureScreenshot'); return send('Page.captureScreenshot', { format: 'png' }, session); })
          .then(function (r) { var b = Buffer.from(r.data, 'base64'); fs.writeFileSync(path.join(OUT, 'cdp-shot1.png'), b); log('SHOT OK bytes=' + b.length); try { child.kill('SIGKILL'); } catch (e) {} process.exit(0); })
          .catch(function (e) { log('SHOT FAIL', e.message); try { child.kill('SIGKILL'); } catch (e2) {} process.exit(3); });
      }
      if (process.env.HOLD) {
        var holdCmd = process.env.HOLD_CMD || 'eval';
        log('HOLD: engine pid ' + child.pid + ' ; cmd=' + holdCmd + ' (no await), holding 60s');
        if (holdCmd === 'none') log('HOLD: cmd=none, sending nothing (attach-only baseline)');
        else if (holdCmd === 'shot') sendRaw({ id: ++nextId, method: 'Page.captureScreenshot', params: { format: 'png' }, sessionId: session });
        else sendRaw({ id: ++nextId, method: 'Runtime.evaluate', params: { expression: '1+1', returnByValue: true }, sessionId: session });
        return sleep(60000).then(function () { try { child.kill('SIGKILL'); } catch (e) {} process.exit(2); });
      }
      if (process.env.NONAV) { log('NONAV: screenshotting about:blank without navigating'); return Promise.resolve(); }
      return navigateBestEffort();
    })
    .then(function () { return sleep(process.env.NONAV ? 2000 : 8000); })
    .then(function () { log('captureScreenshot #1'); return send('Page.captureScreenshot', { format: 'png' }, session); })
    .then(function (r) {
      png1 = Buffer.from(r.data, 'base64');
      fs.writeFileSync(path.join(OUT, 'cdp-shot1.png'), png1);
      log('wrote cdp-shot1.png', png1.length, 'bytes');
    })
    .then(function () { log('startScreencast'); return send('Page.startScreencast', { format: 'jpeg', quality: 60, maxWidth: 800, maxHeight: 800 }, session); })
    .then(function () { return sleep(3000); })
    .then(function () { log('scroll via Input.dispatchMouseEvent mouseWheel'); return send('Input.dispatchMouseEvent', { type: 'mouseWheel', x: 200, y: 300, deltaX: 0, deltaY: 600 }, session); })
    .then(function () { return sleep(2500); })
    .then(function () { log('captureScreenshot #2'); return send('Page.captureScreenshot', { format: 'png' }, session); })
    .then(function (r) {
      png2 = Buffer.from(r.data, 'base64');
      fs.writeFileSync(path.join(OUT, 'cdp-shot2.png'), png2);
      log('wrote cdp-shot2.png', png2.length, 'bytes');
      if (firstFrame) { fs.writeFileSync(path.join(OUT, 'cdp-frame1.jpg'), firstFrame); log('wrote cdp-frame1.jpg', firstFrame.length, 'bytes'); }
      log('RESULT screencastFrames=' + frames + ' shot1=' + png1.length + ' shot2=' + png2.length);
      log('DONE');
      try { child.kill('SIGKILL'); } catch (e) {}
      process.exit(0);
    })
    .catch(function (e) { log('engine pid', child.pid); fail(e.message); });
}

setTimeout(run, 1500);
