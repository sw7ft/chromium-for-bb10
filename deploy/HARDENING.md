# QNX content_shell — feature hardening plan

Goal: re-enable Chromium subsystems disabled for bring-up **before** building a real
browser UI. Each tier keeps the previous regression suite green on Passport:

- `https://example.com`
- `https://www.google.com`
- `https://en.wikipedia.org/wiki/QNX`

Use `./test-regression.sh [tier]` on device (or via `ssh passport`).

## Process cleanup (Passport thermal)

Always stop stray `content_shell` before deploy or regression:

```bash
./kill-content-shell.sh
pidin | grep content_shell   # should show nothing
```

- [`run.sh`](run.sh) traps `EXIT` and kills orphans after each run.
- [`deploy-binary.sh`](deploy-binary.sh) kills first, then atomic `scp` → `content_shell.new` → `mv`.
- Never run regression in the same SSH command as binary deploy.
- Run one test at a time on a cool device; `./test-regression.sh --fast` for example.com only.

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

Each tier removes **exactly one** bring-up workaround so a regression can be
attributed to a single change. Feature flags are re-enabled one at a time
(tiers 2-5) before the process model (tier 6) and render model (tiers 7-8).

| Tier | Single delta vs previous | Risk |
|------|--------------------------|------|
| 1 | Drop `--disable-http2` (HTTP/2 via `cacert.pem`) | Low |
| 2 | Enable `MojoIpcz` (newer Mojo transport) | Low |
| 3 | Enable `NetworkServiceDedicatedThread` (network thread model) | Medium |
| 4 | Enable `Viz` (display compositor host) | Medium |
| 5 | Enable `ServiceWorker` (last; most complex) | Medium |
| 6 | Drop `--single-process` (multi-process; keep `--no-zygote`) | High |
| 7 | `--ozone-platform=headless` → `qnx_screen` (on-screen, software) | High |
| 8 | Drop `--disable-gpu --disable-gpu-compositing` (GPU path) | High |

**Fix-if-broken notes:**

- **Tier 1:** HTTP/2 ALPN / SETTINGS / HPACK on QNX ARM string paths.
- **Tiers 2-5:** Mojo dispatch ordering, thread startup, viz host init on QNX.
- **Tier 6:** `RenderProcessHost` launch, cross-process Mojo, `ChildProcessSecurityPolicy`,
  origin locks at factory creation. Most QNX bypasses assume single-process
  (Shell DCL/DFL callbacks, parse-time dump, stdout routing).
- **Tiers 7-8:** `ui/ozone/platform/qnx_screen/*`, software vs GL path, input events.
  `QNX_GPU_PROBE=1 ./run.sh about:blank` probes EGL.

### Production cleanup (after tier 8)

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
| **0** | PASS (~32s) | PASS (~43s, ~225KB) | PASS (~241KB) | Jun 16 2026; post-commit timeout + cancellable watchdog |
| **1** | PASS | PASS | PASS | +HTTP/2; Jun 16 2026 stable (2/2 runs 3/3) |
| **2** | PASS | PASS | PASS | +MojoIpcz; Jun 16 2026 3/3 |
| **3** | PASS | PASS | PASS | +NetworkServiceDedicatedThread (single-process); Jun 16 2026 3/3 |
| 4 | HANG | HANG | HANG | +Viz (single-process); **init deadlock** before nav commit (27 threads in CONDVAR); watchdog never arms |
| 5 | - | - | - | +ServiceWorker (single-process); blocked on tier 4 |
| 6 | - | - | - | Multi-process; `posix_spawn` launcher wired; blocked on tier 4 |
| 7 | - | - | - | qnx_screen on-screen (software); pending |
| 8 | - | - | - | Full GPU; pending |

Tier 1+ keeps `--ignore-certificate-errors`. Tier 1+ drops `--disable-http2` when `cacert.pem` is present.

### Tier 4 (Viz) blocker — next investigation

Enabling `Viz` in single-process mode deadlocks during startup, **before** the
first navigation commits, so the post-commit `--timeout` watchdog never arms and
the process hangs indefinitely (must be `slay -9`'d). `pidin` shows ~27 threads
parked on `CONDVAR`/`SIGWAITINFO` — a classic init-time wait-for-thread deadlock.

Likely suspects: Viz display-compositor host init waiting on a Mojo channel or
compositor thread that never starts on QNX. Recommended next steps:

1. Run `QNX_TRACE=1 ./run.sh https://example.com` at tier 4 and capture where the
   trace stops (last `QNX:` marker before the hang).
2. Add a **pre-commit safety watchdog** (arm `StartQnxHardWatchdog` at process
   start, not just at navigation commit) so tier 4+ hangs self-terminate during
   bring-up instead of wedging the device.
3. Inspect viz host init (`components/viz/host/*`, `content/browser/compositor/*`)
   for QNX thread-startup assumptions.

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

All three HTTPS URLs produce nonzero DOM output within 90s `--timeout` (armed at
navigation **commit**, not start) + 15s HardWatchdog slack on QNX, no SIGSEGV.
