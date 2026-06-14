# QNX/BB10 Chromium Port - Status & Issues

## Target Device
- **BlackBerry Passport** (BB10 OS, QNX ARM32)
- Accessed via `ssh passport` (169.254.0.1)
- Deploy directory: `/accounts/devuser/berry-deploy/`

## What Works

### Headless DOM Dump (content_shell)
- `about:blank` renders correctly
- **`data:` URLs** fully working (e.g. `data:text/html,<h1>Hello</h1>` dumps DOM correctly)
- **Local HTTP pages** via `http://127.0.0.1:8001/` fully working (HTML + JavaScript)
- Tested with Python HTTP server: `python3.2 -m http.server 8001`

### Test Command
```bash
cd /accounts/devuser/berry-deploy
LD_LIBRARY_PATH=. ./content_shell \
  --no-sandbox --disable-gpu --no-zygote --single-process \
  --disable-features=ServiceWorker,NetworkServiceDedicatedThread,MojoIpcz \
  --ozone-platform=headless --headless --dump-dom \
  'data:text/html,<h1>Hello from BB10</h1>'
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

### 6. libevent select() Timeout Bugs
- **File:** `third_party/libevent/select.c`
- **Issue:** Two broken behaviors: (1) `select()` with `{0, 0}` timeout doesn't reliably report socket readiness. (2) NULL timeout (infinite block) misses wakeup pipe writes, freezing the message loop.
- **Fix:** Cap ALL `select()` calls to max 50ms on QNX, regardless of requested timeout.

### 7. Renderer Not Initialized (Navigation Bypasses)
- **Files:** `content/browser/renderer_host/navigation_request.cc`, `content/browser/renderer_host/render_frame_host_manager.cc`
- **Issue:** QNX-specific `#if` blocks bypassed `GetFrameHostForNavigation()` (which creates the renderer process) and skipped the `IsRenderFrameLive()` check. Commits were sent to a dead renderer.
- **Fix:** Removed the bypasses; the normal `GetFrameHostForNavigation()` path now runs on QNX, which triggers `ReinitializeMainRenderFrame()` → `InitRenderView()` to create the renderer.

### 8. `std::string::find_first_of` Broken on QNX ARM (Compiler/Stdlib Bug)
- **Files:** `net/http/http_util.cc`, `net/base/mime_util.cc`
- **Issue:** QNX ARM's standard library `find_first_of()` returns 0 (wrong) instead of the correct position. This caused HTTP response headers to be misassembled (status line treated as empty, all headers lost, response defaulted to HTTP/0.9 with `application/octet-stream`).
- **Fix:** Replaced all `find_first_of()` / `find_first_not_of()` calls in critical HTTP paths with manual QNX-safe character search loops.
- **Scope:** This is a **systemic** bug affecting any code using `find_first_of` on QNX ARM. More instances likely exist in gzip/chunked decoding and elsewhere.

### 9. `--ozone-platform=headless` Required
- **Issue:** Even with `--headless`, the default `qnx_screen` Ozone platform initializes and spins a polling timer at 8ms, consuming CPU and blocking the message loop.
- **Fix:** Must explicitly pass `--ozone-platform=headless` on the command line.

## In-Progress: External HTTP (e.g. http://example.com)

### Current State (March 2026)
- DNS resolution: **works**
- TCP connect: **works** (thread-pool select() approach)
- HTTP request sent, **200 OK received** with correct headers
- Header parsing: **works** (after find_first_of fix)
- URLLoader receives response, starts MIME sniffing
- **CRASHES** during response body reading (SIGSEGV in `__memcpy_isr` during chunked+gzip body decode)

### Root Cause
The `find_first_of` stdlib bug likely affects more code in the gzip decompression or chunked transfer decoding paths, causing memory corruption that manifests as a SIGSEGV during body data processing.

### Next Steps
1. Search for and fix `find_first_of` / `find_first_not_of` calls in gzip filter and chunked decoder paths
2. Consider a global `find_first_of` override or compiler flag to disable the broken optimization
3. Test with a non-chunked, non-gzipped response (e.g. custom HTTP server returning raw HTML)
4. If body decoding works, test full page rendering

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
