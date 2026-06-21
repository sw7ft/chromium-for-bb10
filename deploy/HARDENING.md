# QNX content_shell — feature hardening plan

Goal: re-enable Chromium subsystems disabled for bring-up **before** building a real
browser UI. Each tier keeps the previous regression suite green on Passport:

- `https://example.com`
- `https://www.google.com`
- `https://en.wikipedia.org/wiki/QNX`

Use `./test-regression.sh [tier]` on device (or via `ssh passport`).

## Current baseline (tier 0 — bring-up)

What `run.sh` uses today:

| Flag / setting | Why it was disabled |
|----------------|---------------------|
| `--single-process --no-zygote` | Multi-process spawn / Mojo IPC untested on QNX |
| `--no-sandbox` | No Linux namespaces on BB10 |
| `--disable-gpu --disable-gpu-compositing` | Compositor crashes / headless path |
| `--disable-features=ServiceWorker,NetworkServiceDedicatedThread,MojoIpcz,Viz` | Hangs, deadlocks, or untested IPC paths |
| `--ozone-platform=headless --headless` | QNX Screen event loop issues during bring-up |
| `--disable-http2` | ALPN → HTTP/2 caused Cloudflare 400 responses |
| `--dump-dom` + parse-time dump (`frame_loader.cc`) | `DidFinishLoad` / JS round-trip never completes on heavy pages |
| `cacert.pem` + optional `--ignore-certificate-errors` | Device lacks system CA store |
| QNX origin commit bypass (`render_frame_host_impl.cc`) | Browser vs renderer origin calc diverges |
| `EnableBatchDispatch()` off (`throttling_url_loader.cc`) | Mojo reentrancy deadlock |
| Async `SendResponseToClient` (`url_loader.cc`) | Same |
| IO-thread read re-arm (`socket_posix.cc`) | Thread-pool `select()` starvation |

## Re-enablement tiers

### Tier 1 — network & TLS hygiene

**Remove:** `--disable-http2`, `--ignore-certificate-errors` (when `SSL_CERT_FILE` set)

**Fix if broken:** HTTP/2 ALPN / SETTINGS / HPACK on QNX ARM string paths; verify
`cacert.pem` covers Cloudflare-backed sites.

**Risk:** Low. Mostly net/ changes already have QNX string workarounds.

### Tier 2 — disabled features (one at a time)

Re-enable each feature individually; update `QNX_DISABLED_FEATURES` in `run.sh`:

1. `MojoIpcz` — newer Mojo transport; may affect cross-process later
2. `NetworkServiceDedicatedThread` — network service thread model
3. `Viz` — display compositor host (needed before on-screen rendering)
4. `ServiceWorker` — last; most complex

**Fix if broken:** Mojo dispatch ordering, thread startup, viz host init on QNX.

### Tier 3 — multi-process

**Remove:** `--single-process` (keep `--no-zygote` until zygote works)

**Fix if broken:** `RenderProcessHost` launch on QNX, `/proc` or equivalent,
cross-process Mojo, `ChildProcessSecurityPolicy`, origin locks at factory creation.

**Risk:** High. Most QNX bypasses assume single-process (e.g. Shell DCL/DFL callbacks).

### Tier 4 — on-screen rendering

**Remove:** `--headless`, switch `--ozone-platform=headless` → `qnx_screen`

**Remove (in order):** `--disable-gpu-compositing`, then `--disable-gpu`

**Fix if broken:** `ui/ozone/platform/qnx_screen/*`, software vs GL path, input events.

**Risk:** High. Required for a real browser window.

### Tier 5 — production cleanup

- Remove parse-time `--dump-dom` shortcut; restore `DidFinishLoad` / Shell timeout path
- Remove or narrow QNX origin commit bypass (fix root cause in origin calculation)
- Strip `--qnx-trace` diagnostics (or gate behind `--qnx-trace` only)
- Re-enable `EnableBatchDispatch()` if Mojo ordering is fixed globally

### Permanent on BB10 (likely)

| Item | Notes |
|------|-------|
| `--no-sandbox` | Unless a QNX-specific sandbox is implemented |
| `--no-zygote` | Until fork-based zygote is viable on QNX |

## Regression results (Passport, bundle v1)

| Tier | example.com | google.com | wikipedia/QNX | Notes |
|------|-------------|------------|---------------|-------|
| 0 | PASS | PASS | PASS | Baseline; `./test-regression.sh 0` |
| 1 | PASS | PASS | FAIL (flaky) | HTTP/2 enabled; wiki may exit non-zero intermittently |
| 2 | PASS | FAIL (flaky) | FAIL (flaky) | +MojoIpcz; re-run recommended |
| 3 | FAIL | FAIL | FAIL | Multi-process: HardWatchdog; real `posix_spawn` launcher wired |
| 4 | FAIL | FAIL | FAIL | qnx_screen headful: HardWatchdog on all URLs |
| 5 | (not run) | | | Full GPU; use `QNX_GPU_PROBE=1 ./run.sh about:blank` to probe EGL |

Tier 1+ keeps `--ignore-certificate-errors`. Tier 1+ drops `--disable-http2` when `cacert.pem` is present.

## Berry daemon (experimental)

TCP command channel on `127.0.0.1:8767` (`LOAD <url>` / `QUIT`). Framed stdout: `@BERRY DOM <len>\\n<html>\\n@BERRY END`.

- `./test-daemon.sh` waits for the TCP port (stderr buffering makes log-grep unreliable).
- Berry Proxy: one-shot default (`BERRY_USE_DAEMON=0`); set `BERRY_USE_DAEMON=1` to try daemon mode.
- Cold start to listening port can take 30–90s on Passport.

## Usage

```bash
# Default bring-up (tier 0)
./run.sh https://example.com

# Try tier 1 flags
QNX_HARDENING_TIER=1 ./run.sh https://www.google.com

# Full regression for a tier
./test-regression.sh 1
```

## Success criteria per tier

All three HTTPS URLs produce nonzero DOM output within 90s, no SIGSEGV, no hang past
hard watchdog (tier 0 only).
