# QNX/BB10 Chromium Port - Status & Issues

## Target Device
- **BlackBerry Passport** (BB10 OS, QNX ARM32)
- Accessed via `ssh passport` (169.254.0.1)
- Deploy directory: `/accounts/devuser/berry-deploy/`

## What Works

### Headless DOM Dump (content_shell)
- `about:blank` renders correctly
- **Local HTTP pages** via `http://127.0.0.1:8001/` fully working (HTML + JavaScript)
- Tested with Python HTTP server: `python3.2 -m http.server 8001`

### Test Command
```bash
cd /accounts/devuser/berry-deploy
LD_LIBRARY_PATH=.:$LD_LIBRARY_PATH ./content_shell \
  --no-sandbox --disable-gpu --no-zygote --single-process \
  --disable-features=ServiceWorker,NetworkServiceDedicatedThread,MojoIpcz \
  --headless --dump-dom http://127.0.0.1:8001/
```

## QNX-Specific Bugs Fixed

### 1. Clipboard Hang
- **File:** `content/browser/renderer_host/render_view_host_impl.cc`
- **Issue:** `ui::Clipboard::IsSupportedClipboardBuffer()` hangs indefinitely on QNX Ozone.
- **Fix:** Guarded with `#if BUILDFLAG(IS_OZONE) && !BUILDFLAG(IS_QNX)`.

### 2. TRACE_EVENT Hangs
- **Files:** `render_view_host_impl.cc`, `render_frame_host_manager.cc`, `web_contents_impl.cc`, `message_pump_libevent.cc`
- **Issue:** `TRACE_EVENT` macros cause hangs on QNX (likely perfetto/tracing subsystem incompatibility).
- **Fix:** Wrapped all with `#if !BUILDFLAG(IS_QNX)`.

### 3. URL Path Stripping (Compiler Bug)
- **File:** `url/third_party/mozilla/url_parse.cc`
- **Issue:** `ParsePath()` template silently drops the path component on QNX ARM (e.g. `/simple.html` becomes `/`). Likely a QNX ARM codegen bug.
- **Fix:** Bypass `ParsePath()` on QNX in `DoParseAfterScheme()` and set `parsed->path = full_path` directly.

### 4. DOCTYPE Crash (Compiler Bug)
- **File:** `third_party/blink/renderer/core/html/parser/html_document_parser.cc`
- **Issue:** Lowercase `<!doctype html>` causes SIGSEGV in `strlen` during HTML tokenization on QNX ARM. Uppercase `<!DOCTYPE html>` works fine.
- **Fix:** Pre-process HTML input in `HTMLDocumentParser::Append()` to normalize `<!doctype` to `<!DOCTYPE` before tokenization.

### 5. libevent poll() Broken for External TCP
- **Files:** `third_party/libevent/qnx/config.h`, `third_party/libevent/qnx/event-config.h`
- **Issue:** libevent's `poll()` backend blocks indefinitely on QNX for TCP sockets connected to external IPs, ignoring its timeout.
- **Fix:** Disabled `poll()` backend (`#undef HAVE_POLL`) to force `select()` backend.

### 6. libevent select() Zero-Timeout Bug
- **File:** `third_party/libevent/select.c`
- **Issue:** `select()` with `{0, 0}` timeout doesn't reliably report socket readiness on QNX.
- **Fix:** Enforce minimum 50ms timeout when `tv == {0,0}` and there are active file descriptors.

## In-Progress: External HTTP (e.g. http://example.com)

### Current State
- TCP connect to external IPs **works** (confirmed by traces)
- HTTP request is **sent** successfully (410 bytes written)
- Server responds with **710 bytes** (HTTP 200 OK, chunked gzip, keep-alive)
- Response is complete (chunked encoding terminates correctly with `0\r\n\r\n`)

### The Problem
QNX `select()` needs a long timeout (~500ms+) to detect read readiness on external TCP sockets. But blocking the IO thread for that long prevents Chromium's Mojo IPC from dispatching messages, creating a deadlock:
1. Data arrives on the socket (710 bytes)
2. `select()` blocks IO thread waiting for "more" data
3. Mojo can't deliver already-received data to the renderer
4. Renderer can't process data, so HTTP layer stays stuck

### Latest Approach (Untested - Device Went Offline)
- **Files:** `net/socket/socket_posix.cc`, `net/socket/socket_posix.h`
- Offload blocking `select()` to a **thread pool thread** via `base::ThreadPool::PostTask`
- IO thread stays free for Mojo IPC
- On `select()` returning readable, post result back to IO thread via `PostTask`
- Non-blocking connect polling via `QnxPollForConnect()` (task-based, 20ms intervals)

### What Needs Testing
1. Deploy latest binary to Passport and test `http://example.com`
2. If thread pool approach crashes device, investigate (may need to check thread safety of fd access)
3. If it works, test DNS resolution for other domains
4. Test more complex sites

## Git History (branch: qnx-bb10)
```
25632e30a3 QNX/BB10: Fix external TCP connections with non-blocking socket polling
3e2798d406 QNX/BB10: Fix DOCTYPE crash, URL parsing, and stabilize HTTP rendering
e402c0d7c4 QNX/BB10: New platform-specific files
960a444606 QNX/BB10: Content shell and headless dump-dom for HTTP pages
b93064e226 QNX/BB10: Components, UI, media, and remaining adaptations
8e9a38aef5 QNX/BB10: Blink renderer, parser, and platform fixes
1cb33f6bc2 QNX/BB10: Content browser and navigation fixes
391436989c QNX/BB10: Network, IPC, and Mojo adaptations
90dca5b231 QNX/BB10: Fix cross-thread task delivery in SequenceManager
f901bd92f9 QNX/BB10: Build system and base platform adaptation
```

## BAR File (Native App) Status
- BAR packaging attempted but native windowed app crashes on launch
- Shell/headless mode is the current working path
- BAR investigation documented separately; needs Ozone `qnx_screen` platform debugging
