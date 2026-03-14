# QNX/BB10 Chromium Port - Working Milestones

Each milestone below is a confirmed working state with the exact command used
and the output captured. Timestamps are when the test was actually run.

---

## Milestone 1: `data:` URL Rendering (HTML)

**Date:** 2026-03-13
**Status:** WORKING

### Command
```bash
cd /accounts/devuser/berry-deploy
LD_LIBRARY_PATH=. ./content_shell \
  --no-sandbox --disable-gpu --no-zygote --single-process \
  --disable-features=ServiceWorker,NetworkServiceDedicatedThread,MojoIpcz \
  --ozone-platform=headless --headless --dump-dom \
  'data:text/html,<h1>Hello from BB10</h1>'
```

### Output (stdout)
```
<html><head></head><body><h1>Hello from BB10</h1></body></html>
```

### What This Proves
- Chromium's HTML parser (Blink) works on QNX ARM32
- DOM construction and serialization work
- The headless Ozone platform works
- Single-process mode works
- `--dump-dom` correctly waits for load and outputs the DOM tree
- `data:` URL scheme is handled correctly

---

## Milestone 2: `about:blank` Rendering

**Date:** 2026-03 (confirmed in earlier session)
**Status:** WORKING

### Command
```bash
cd /accounts/devuser/berry-deploy
LD_LIBRARY_PATH=. ./content_shell \
  --no-sandbox --disable-gpu --no-zygote --single-process \
  --disable-features=ServiceWorker,NetworkServiceDedicatedThread,MojoIpcz \
  --ozone-platform=headless --headless --dump-dom \
  'about:blank'
```

### Output (stdout)
```
<html><head></head><body></body></html>
```

### What This Proves
- Minimal browser initialization and navigation pipeline works

---

## Milestone 3: Local HTTP Server Rendering (HTML + JavaScript)

**Date:** 2026-03 (confirmed in earlier session)
**Status:** WORKING

### Prerequisites
On the BlackBerry device, start a local HTTP server:
```bash
cd /accounts/devuser
echo '<html><body><p>Static page</p></body></html>' > index.html
python3.2 -m http.server 8001 &
```

### Command
```bash
cd /accounts/devuser/berry-deploy
LD_LIBRARY_PATH=. ./content_shell \
  --no-sandbox --disable-gpu --no-zygote --single-process \
  --disable-features=ServiceWorker,NetworkServiceDedicatedThread,MojoIpcz \
  --ozone-platform=headless --headless --dump-dom \
  'http://127.0.0.1:8001/'
```

### Output (stdout)
```
<html><head></head><body><p>Static page</p></body></html>
```

### What This Proves
- TCP socket connections work for localhost
- HTTP request/response cycle works
- Network stack (URLLoader, HttpNetworkTransaction) handles local connections
- Full pipeline: DNS → TCP → HTTP → parse → render → dump-dom

---

## Milestone 4: External HTTP (e.g. http://example.com)

**Date:** NOT YET ACHIEVED
**Status:** CRASHES (SIGSEGV during chunked+gzip body decode)

### Command
```bash
cd /accounts/devuser/berry-deploy
LD_LIBRARY_PATH=. ./content_shell \
  --no-sandbox --disable-gpu --no-zygote --single-process \
  --disable-features=ServiceWorker,NetworkServiceDedicatedThread,MojoIpcz \
  --ozone-platform=headless --headless --dump-dom \
  'http://example.com'
```

### What Works So Far
- DNS resolution resolves `example.com`
- TCP connect succeeds (via thread-pool select() workaround)
- HTTP 200 OK received with correct headers (after find_first_of fix)
- Response body reading begins

### What Fails
- SIGSEGV in `__memcpy_isr` during chunked+gzip body decode
- Suspected root cause: more `find_first_of` stdlib bugs in decompression path

---

## Build & Deploy Reference

### Build (on host)
```bash
cd /root/chromium/src
autoninja -C out/ArmQnx content_shell
```

### Package
```bash
cd /root/chromium/src
./build_scripts/package-content-shell.sh
```

### Deploy
```bash
scp berry-content-shell-latest.tar.gz passport:/accounts/devuser/
ssh passport "cd /accounts/devuser && tar xzf berry-content-shell-latest.tar.gz"
```
Or if device lacks gzip:
```bash
cd /tmp && tar xzf /root/chromium/src/berry-content-shell-latest.tar.gz
scp -r berry-content-shell-latest/* passport:/accounts/devuser/berry-deploy/
```

### Common Flags
```
--no-sandbox --disable-gpu --no-zygote --single-process
--disable-features=ServiceWorker,NetworkServiceDedicatedThread,MojoIpcz
--ozone-platform=headless --headless --dump-dom
```
