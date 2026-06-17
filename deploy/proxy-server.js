#!/usr/bin/env node
/**
 * Berry Proxy — headless Chromium renderer for the BB10 stock browser.
 *
 * Uses a persistent content_shell daemon (--berry-daemon) when available so
 * only the first page pays the cold-start cost (~30s). Subsequent loads reuse
 * the warm process (~5–15s depending on page).
 */

var http = require('http');
var net = require('net');
var url = require('url');
var cp = require('child_process');
var fs = require('fs');
var path = require('path');

var PORT = parseInt(process.env.BERRY_PROXY_PORT || '8765', 10);
var HOST = process.env.BERRY_PROXY_HOST || '127.0.0.1';
var DEPLOY_DIR = process.env.BERRY_DEPLOY ||
    path.dirname(fs.realpathSync(__filename));
var RENDER_TIMEOUT_MS = parseInt(
    process.env.BERRY_PROXY_TIMEOUT_MS || '120000', 10);
var MAX_BODY_BYTES = 8 * 1024 * 1024;
var USE_DAEMON = process.env.BERRY_USE_DAEMON === '1';
var DAEMON_CMD_PORT = parseInt(process.env.BERRY_DAEMON_CMD_PORT || '8767', 10);

function log(msg) {
  process.stderr.write('[berry-proxy] ' + msg + '\n');
}

function isAllowedTarget(href) {
  if (!href || typeof href !== 'string') {
    return false;
  }
  var lower = href.toLowerCase();
  return lower.indexOf('http://') === 0 || lower.indexOf('https://') === 0;
}

function htmlEscape(s) {
  return String(s)
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;');
}

function landingPage(queryUrl) {
  var q = queryUrl ? htmlEscape(queryUrl) : 'https://example.com';
  return '<!DOCTYPE html>\n' +
      '<html><head><meta charset="UTF-8">' +
      '<meta name="viewport" content="width=device-width, initial-scale=1">' +
      '<title>Berry Proxy</title>' +
      '<style>body{font-family:sans-serif;margin:1em;max-width:40em}' +
      'input[type=text]{width:100%;box-sizing:border-box;padding:8px}' +
      'button{padding:8px 16px;margin-top:8px}' +
      '.note{color:#666;font-size:0.9em;margin-top:1.5em}</style></head><body>' +
      '<h1>Berry Proxy</h1>' +
      '<p>Modern pages rendered by Chromium headless, shown in this browser.</p>' +
      '<form action="/render" method="get">' +
      '<label>URL<br><input type="text" name="url" value="' + q + '"></label><br>' +
      '<button type="submit">Open</button></form>' +
      '<p class="note">First load may take 30–60 seconds while Chromium starts. ' +
      'Later loads reuse the warm renderer and are much faster. JavaScript on ' +
      'target sites runs in Chromium, not here — layout and text usually work.</p>' +
      '<ul>' +
      '<li><a href="/render?url=' +
      encodeURIComponent('https://example.com') + '">example.com</a></li>' +
      '<li><a href="/render?url=' +
      encodeURIComponent('https://en.wikipedia.org/wiki/QNX') + '">Wikipedia: QNX</a></li>' +
      '</ul></body></html>';
}

function errorPage(status, title, detail) {
  var body = '<!DOCTYPE html><html><head><meta charset="UTF-8"><title>' +
      htmlEscape(title) + '</title></head><body>' +
      '<h1>' + htmlEscape(title) + '</h1><p>' + htmlEscape(detail) +
      '</p><p><a href="/">Back</a></p></body></html>';
  return {status: status, headers: {'Content-Type': 'text/html; charset=utf-8'},
    body: body};
}

function injectBanner(html, targetUrl) {
  var banner = '<div style="background:#222;color:#fff;padding:8px 12px;' +
      'font-family:sans-serif;font-size:14px;position:sticky;top:0;z-index:99999">' +
      'Berry Proxy &middot; ' + htmlEscape(targetUrl) +
      ' &middot; <a style="color:#9cf" href="/">Home</a></div>';
  if (html.indexOf('<body') !== -1) {
    return html.replace(/<body([^>]*)>/i, '<body$1>' + banner);
  }
  return banner + html;
}

function deployEnv() {
  var env = {};
  var k;
  for (k in process.env) {
    if (process.env.hasOwnProperty(k)) {
      env[k] = process.env[k];
    }
  }
  env.LD_LIBRARY_PATH = DEPLOY_DIR +
      (env.LD_LIBRARY_PATH ? ':' + env.LD_LIBRARY_PATH : '');
  env.QNX_BERRY_DAEMON = '1';
  env.QNX_BERRY_DAEMON_MARKER = path.join(DEPLOY_DIR, 'berry-daemon.mode');
  env.QNX_BERRY_CMD_PORT = String(DAEMON_CMD_PORT);
  if (fs.existsSync(path.join(DEPLOY_DIR, 'cacert.pem'))) {
    env.SSL_CERT_FILE = path.join(DEPLOY_DIR, 'cacert.pem');
  }
  return env;
}

function BerryDaemonClient(deployDir) {
  this.deployDir = deployDir;
  this.child = null;
  this.buffer = Buffer.alloc(0);
  this.queue = [];
  this.active = null;
  this.parseState = 'line';
  this.bodyLen = 0;
  this.bodyChunks = [];
  this.bodyReceived = 0;
  this.cache = Object.create(null);
  this.restarting = false;
}

BerryDaemonClient.prototype.hasDaemonScript = function() {
  return fs.existsSync(path.join(this.deployDir, 'run-daemon.sh')) &&
      fs.existsSync(path.join(this.deployDir, 'content_shell'));
};

BerryDaemonClient.prototype.startChild = function() {
  var self = this;
  var runDaemon = path.join(this.deployDir, 'run-daemon.sh');
  log('starting berry-daemon');
  try {
    fs.writeFileSync(path.join(this.deployDir, 'berry-daemon.mode'), '');
  } catch (e) {}
  this.daemonReady = false;
  this.child = cp.spawn('sh', [runDaemon], {
    cwd: this.deployDir,
    env: deployEnv(),
    stdio: ['ignore', 'pipe', 'pipe']
  });
  this.buffer = Buffer.alloc(0);
  this.parseState = 'line';
  this.bodyLen = 0;
  this.bodyChunks = [];
  this.bodyReceived = 0;

  this.child.stdout.on('data', function(chunk) {
    self.onStdout(chunk);
  });
  this.child.stderr.on('data', function(chunk) {
    var text = chunk.toString('utf8');
    if (text.indexOf('berry-daemon listening') !== -1) {
      self.daemonReady = true;
    }
    process.stderr.write(chunk);
  });
  this.child.on('error', function(err) {
    self.failActive(err);
  });
  this.child.on('close', function(code) {
    log('berry-daemon exited ' + code);
    self.child = null;
    if (self.active) {
      self.failActive(new Error('berry-daemon exited unexpectedly'));
    }
    if (!self.restarting && self.queue.length) {
      self.restarting = true;
      setTimeout(function() {
        self.restarting = false;
        self.startChild();
        self.pumpQueue();
      }, 1000);
    }
  });
};

BerryDaemonClient.prototype.ensureChild = function() {
  if (!this.child) {
    this.startChild();
  }
};

BerryDaemonClient.prototype.failActive = function(err) {
  if (!this.active) {
    return;
  }
  var cb = this.active.callback;
  this.active = null;
  this.parseState = 'line';
  this.bodyLen = 0;
  this.bodyChunks = [];
  this.bodyReceived = 0;
  cb(err);
  this.pumpQueue();
};

BerryDaemonClient.prototype.finishActive = function(html) {
  if (!this.active) {
    return;
  }
  var item = this.active;
  this.active = null;
  this.parseState = 'line';
  this.bodyLen = 0;
  this.bodyChunks = [];
  this.bodyReceived = 0;
  this.cache[item.url] = html;
  item.callback(null, html);
  this.pumpQueue();
};

BerryDaemonClient.prototype.readLine = function() {
  var nl = this.buffer.indexOf(0x0a);
  if (nl === -1) {
    return null;
  }
  var line = this.buffer.slice(0, nl).toString('utf8');
  this.buffer = this.buffer.slice(nl + 1);
  return line;
};

BerryDaemonClient.prototype.onStdout = function(chunk) {
  this.buffer = Buffer.concat([this.buffer, chunk]);

  while (true) {
    if (this.parseState === 'line') {
      var line = this.readLine();
      if (line === null) {
        return;
      }
      if (line.indexOf('@BERRY DOM ') === 0) {
        this.bodyLen = parseInt(line.slice(11), 10);
        if (isNaN(this.bodyLen) || this.bodyLen < 0 ||
            this.bodyLen > MAX_BODY_BYTES) {
          this.failActive(new Error('invalid DOM length from daemon'));
          return;
        }
        this.bodyChunks = [];
        this.bodyReceived = 0;
        this.parseState = 'body';
        continue;
      }
      if (line.indexOf('@BERRY ERR ') === 0) {
        this.pendingError = line.slice(11);
        this.parseState = 'end';
        continue;
      }
      if (line.indexOf('@BERRY END') === 0) {
        continue;
      }
      continue;
    }

    if (this.parseState === 'body') {
      var need = this.bodyLen - this.bodyReceived;
      if (this.buffer.length < need) {
        return;
      }
      if (need > 0) {
        this.bodyChunks.push(this.buffer.slice(0, need));
        this.buffer = this.buffer.slice(need);
        this.bodyReceived += need;
      }
      this.parseState = 'end';
      continue;
    }

    if (this.parseState === 'end') {
      var endLine = this.readLine();
      if (endLine === null) {
        return;
      }
      if (endLine.indexOf('@BERRY END') !== 0) {
        this.failActive(new Error('expected @BERRY END, got: ' + endLine));
        return;
      }
      if (this.pendingError) {
        var errMsg = this.pendingError;
        this.pendingError = null;
        this.failActive(new Error(errMsg));
        return;
      }
      var html = Buffer.concat(this.bodyChunks).toString('utf8');
      this.finishActive(html);
      return;
    }
  }
};

BerryDaemonClient.prototype.sendCommand = function(cmd, callback) {
  var self = this;
  var attempts = 0;
  var maxAttempts = 120;

  function tryConnect() {
    attempts++;
    var sock = net.connect({host: '127.0.0.1', port: DAEMON_CMD_PORT}, function() {
      sock.write(cmd + '\n');
      sock.end();
      callback(null);
    });
    sock.on('error', function(err) {
      if (attempts < maxAttempts && self.child) {
        setTimeout(tryConnect, 500);
        return;
      }
      callback(err);
    });
  }
  tryConnect();
};

BerryDaemonClient.prototype.pumpQueue = function() {
  if (this.active || !this.queue.length) {
    return;
  }
  this.ensureChild();
  if (!this.child) {
    return;
  }
  var item = this.queue.shift();
  this.active = item;
  log('daemon load ' + item.url);
  var timer = setTimeout(function() {
    if (berryDaemon.active === item) {
      berryDaemon.failActive(new Error(
          'Render timed out after ' + RENDER_TIMEOUT_MS + 'ms'));
      if (berryDaemon.child) {
        berryDaemon.child.kill('SIGTERM');
        berryDaemon.child = null;
      }
    }
  }, RENDER_TIMEOUT_MS);
  item.timer = timer;
  this.sendCommand('LOAD ' + item.url, function(err) {
    if (err) {
      clearTimeout(timer);
      self.active = null;
      item.callback(err);
      self.pumpQueue();
    }
  });
};

BerryDaemonClient.prototype.render = function(targetUrl, callback) {
  if (this.cache[targetUrl]) {
    log('cache hit ' + targetUrl);
    callback(null, this.cache[targetUrl]);
    return;
  }
  var self = this;
  this.queue.push({
    url: targetUrl,
    callback: function(err, html) {
      if (self.active && self.active.url === targetUrl && self.active.timer) {
        clearTimeout(self.active.timer);
      }
      callback(err, html);
    }
  });
  this.pumpQueue();
};

function renderWithContentShellOnce(targetUrl, callback) {
  var runSh = path.join(DEPLOY_DIR, 'run.sh');
  var shellBin = path.join(DEPLOY_DIR, 'content_shell');
  if (!fs.existsSync(runSh) || !fs.existsSync(shellBin)) {
    callback(new Error('content_shell not found in ' + DEPLOY_DIR));
    return;
  }

  log('one-shot render ' + targetUrl);
  var args = [runSh, targetUrl, '--timeout=90000'];
  var child = cp.spawn('sh', args, {
    cwd: DEPLOY_DIR,
    env: deployEnv(),
    stdio: ['ignore', 'pipe', 'pipe']
  });

  var stdout = [];
  var stderr = [];
  var stdoutLen = 0;
  var killed = false;

  var timer = setTimeout(function() {
    killed = true;
    child.kill('SIGTERM');
    callback(new Error('Render timed out after ' + RENDER_TIMEOUT_MS + 'ms'));
  }, RENDER_TIMEOUT_MS);

  child.stdout.on('data', function(chunk) {
    stdoutLen += chunk.length;
    if (stdoutLen > MAX_BODY_BYTES) {
      killed = true;
      child.kill('SIGTERM');
      callback(new Error('Response too large'));
      return;
    }
    stdout.push(chunk);
  });
  child.stderr.on('data', function(chunk) {
    stderr.push(chunk);
  });

  child.on('error', function(err) {
    clearTimeout(timer);
    callback(err);
  });

  child.on('close', function(code) {
    clearTimeout(timer);
    if (killed) {
      return;
    }
    var html = Buffer.concat(stdout).toString('utf8');
    if (code !== 0 && !html) {
      var errText = Buffer.concat(stderr).toString('utf8').slice(-500);
      callback(new Error('content_shell exit ' + code + ': ' + errText));
      return;
    }
    if (!html || html.length < 10) {
      callback(new Error('Empty DOM from content_shell'));
      return;
    }
    callback(null, html);
  });
}

var berryDaemon = new BerryDaemonClient(DEPLOY_DIR);
var daemonAvailable = USE_DAEMON && berryDaemon.hasDaemonScript();

function renderPage(targetUrl, callback) {
  if (daemonAvailable) {
    berryDaemon.render(targetUrl, callback);
    return;
  }
  renderWithContentShellOnce(targetUrl, callback);
}

function sendJson(res, status, obj) {
  var body = JSON.stringify(obj);
  res.writeHead(status, {
    'Content-Type': 'application/json; charset=utf-8',
    'Content-Length': Buffer.byteLength(body)
  });
  res.end(body);
}

function sendHtml(res, status, html) {
  res.writeHead(status, {
    'Content-Type': 'text/html; charset=utf-8',
    'Content-Length': Buffer.byteLength(html, 'utf8')
  });
  res.end(html);
}

var server = http.createServer(function(req, res) {
  var parsed = url.parse(req.url, true);
  var pathname = parsed.pathname || '/';

  if (req.method !== 'GET' && req.method !== 'HEAD') {
    sendJson(res, 405, {error: 'Method not allowed'});
    return;
  }

  if (pathname === '/health') {
    sendJson(res, 200, {
      ok: true,
      deploy: DEPLOY_DIR,
      port: PORT,
      daemon: daemonAvailable
    });
    return;
  }

  if (pathname === '/' || pathname === '/index.html') {
    var page = landingPage(parsed.query.url || '');
    if (req.method === 'HEAD') {
      res.writeHead(200, {'Content-Type': 'text/html; charset=utf-8'});
      res.end();
      return;
    }
    sendHtml(res, 200, page);
    return;
  }

  if (pathname === '/render') {
    var target = parsed.query.url;
    if (!isAllowedTarget(target)) {
      sendHtml(res, 400, errorPage(400, 'Bad URL',
          'Provide ?url=https://... or http://...').body);
      return;
    }
    renderPage(target, function(err, html) {
      if (err) {
        log('fail: ' + err.message);
        var status = err.message.indexOf('timed out') !== -1 ? 504 : 502;
        sendHtml(res, status, errorPage(status, 'Render failed',
            err.message).body);
        return;
      }
      html = injectBanner(html, target);
      if (req.method === 'HEAD') {
        res.writeHead(200, {'Content-Type': 'text/html; charset=utf-8'});
        res.end();
        return;
      }
      sendHtml(res, 200, html);
    });
    return;
  }

  sendHtml(res, 404, errorPage(404, 'Not found',
      'Try / or /render?url=https://example.com').body);
});

server.listen(PORT, HOST, function() {
  log('listening on http://' + HOST + ':' + PORT);
  log('deploy dir: ' + DEPLOY_DIR);
  log('daemon mode: ' + (daemonAvailable ? 'on' : 'off'));
  if (daemonAvailable) {
    berryDaemon.ensureChild();
  }
  log('Open in BB10 browser: http://127.0.0.1:' + PORT + '/');
});

process.on('SIGTERM', function() {
  berryDaemon.sendCommand('QUIT', function() {
    process.exit(0);
  });
  setTimeout(function() {
    process.exit(0);
  }, 2000);
});
