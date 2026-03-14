# Chromium for BB10 - content_shell Working Milestones

All tests run on BlackBerry Passport (QNX 8.0.0, ARMv7-A) in headless
`--dump-dom` mode with single-process.

Command template:
```
LD_LIBRARY_PATH=. ./content_shell --no-sandbox --disable-gpu --no-zygote \
  --single-process --disable-features=ServiceWorker,NetworkServiceDedicatedThread,MojoIpcz \
  --ozone-platform=headless --headless --dump-dom '<URL>'
```

---

## Milestone 1 - data: URL rendering (2025-03-14)

**URL**: `data:text/html,<h1>Hello from BB10</h1>`

**Result**: SUCCESS - Full DOM dump produced.

**Output**: `<html><head></head><body><h1>Hello from BB10</h1></body></html>`

**Files**: `milestone1_data_url_stdout.txt`, `milestone1_data_url_stderr.txt`

**Key fixes required**:
- `--ozone-platform=headless` to prevent QnxScreenEventSource polling loop
- `select()` timeout capping in libevent (50ms max on QNX)
- TRACE_EVENT guard in message_pump_libevent.cc
- Renderer creation bypasses removed (navigation_request.cc, render_frame_host_manager.cc)

---

## Milestone 2 - Local HTTP rendering (2025-03-14)

**URL**: `http://127.0.0.1:8001/index.html` (Python HTTP server on device)

**Result**: SUCCESS - Full DOM dump produced.

**Output**:
```
<html><head><title>Test Page</title></head><body><h1>Local HTTP Works!</h1><p>Served from BB10 Passport</p>
</body></html>
```

**Files**: `milestone2_local_http_stdout.txt`, `milestone2_local_http_stderr.txt`

**Additional fixes required (beyond Milestone 1)**:
- QNX ARM `std::string::find_first_of` bug: manual loop replacements in http_util.cc, mime_util.cc
- QNX ARM `std::string::find_first_not_of` bug: manual loop in http_response_headers.cc (`RemoveLeadingSpaces`)
- QNX ARM `std::string::find(char, pos)` bug: QnxStrFind helper in net_ipc_param_traits.cc
- `HttpResponseHeaders` Mojo deserialization: bypass Builder API, use raw constructor
- `EnableBatchDispatch()` disabled on QNX in throttling_url_loader.cc
- `Accept-Encoding: identity` forced on QNX to avoid gzip/deflate
- QnxPollForRead/QnxReadSelectDone retry mechanism (12 retries, 5s each)

---

## Milestone 3 - External HTTP rendering (2025-03-14)

**URL**: `http://example.com`

**Result**: SUCCESS - Full DOM dump produced. Zero crashes, zero timeouts.

**Output**:
```
<html><head><title>Example Domain</title>...
<h1>Example Domain</h1>
<p>This domain is for use in documentation examples...</p>
...</html>
```

**Files**: `milestone3_external_http_stdout.txt`, `milestone3_external_http_stderr.txt`

**Additional fixes required (beyond Milestone 2)**:
- `base::StringPiece::find()` bug in HttpChunkedDecoder: QnxFindChar helper
- All cumulative stdlib workarounds functioning end-to-end for real-world HTTP responses

---

## Milestone 4 - HTTPS rendering (2026-03-14)

**URL**: `https://example.com`

**Result**: SUCCESS - Full DOM dump produced. Zero crashes, clean exit.

**Extra flags**: `--ignore-certificate-errors --disable-http2`

**Output**:
```
<html><head><title>Example Domain</title>...
<h1>Example Domain</h1>
<p>This domain is for use in documentation examples...</p>
...</html>
```

**Files**: `milestone4_https_stdout.txt`, `milestone4_https_stderr.txt`

**Additional fixes required (beyond Milestone 3)**:
- `--disable-http2` to force HTTP/1.1 (HTTP/2 ALPN negotiation causes 400 from Cloudflare)
- `--ignore-certificate-errors` to bypass cert validation (no root CA store on device)
- Guard `EnterChildTraceEvent("OnResponseStarted", ...)` with `#if !BUILDFLAG(IS_QNX)` to prevent SIGSEGV in strlen from trace event string handling
- TLS 1.3 handshake and BoringSSL work correctly on QNX ARM
- `CertVerifierServiceFactory` Mojo IPC works end-to-end
