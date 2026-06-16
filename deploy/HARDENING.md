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
| 4 | FLAKY | FLAKY | FLAKY | +Viz (single-process); **non-deterministic** ~40% pass / ~50% hang / partial; boot watchdog contains failures. See below. |
| 5 | - | - | - | +ServiceWorker (single-process); blocked on tier 4 |
| 6 | - | - | - | Multi-process; `posix_spawn` launcher wired; blocked on tier 4 |
| 7 | - | - | - | qnx_screen on-screen (software); pending |
| 8 | - | - | - | Full GPU; pending |

Tier 1+ keeps `--ignore-certificate-errors`. Tier 1+ drops `--disable-http2` when `cacert.pem` is present.

### Tier 4 (Viz) — investigation findings (Jun 16 2026)

Tier 4 is **not** a hard deadlock; it is a **non-deterministic race**. Over
repeated runs we see ~40% success (full DOM dump), ~50% hang, occasional partial
dumps. The boot watchdog (armed at process start) now contains every failure
mode, so the device no longer wedges and `--qnx-trace` runs are safe to repeat.

Two distinct symptoms, **one shared hot spot** — the out-of-process
`cert_verifier.mojom.CertVerifierService` mojo path:

1. **Crash (tracing-only artifact, FIXED).** Earlier SIGSEGV in `strlen` via QNX
   libc `_Putfld` (printf) with `R0=0x1f`. Root cause: our `QNX_TRACE_FMT(...
   iface=%s ...)` traces in `mojo/.../connector.cc` and
   `interface_endpoint_client.cc` printed `Connector::interface_name_`, which is a
   **garbage/dangling pointer** during racy cross-thread dispatch. The null-guard
   (`x ? x : "?"`) did not catch a non-null garbage value, and QNX libc `%s` does
   not tolerate it (unlike glibc). Fixed with `base::QnxSafeStr()` (range-checks
   the pointer, prints `<bad>`); the crash only ever occurred **with**
   `--qnx-trace`. Confirmed: 0 crashes in 16 traced runs post-fix, with
   `iface=<bad>` appearing when the garbage pointer is hit.

2. **Hang (real blocker, open).** Without tracing, a worker thread (e.g. tid 10,
   the `MultiThreadedCertVerifier` pool) parks forever on a heap-allocated
   `ConditionVariable` while the main thread sits in `SIGWAITINFO`. The garbage
   `interface_name_` from (1) is strong evidence that the CertVerifierService
   mojo endpoint/`Connector` is used in a torn/use-after-free state under the
   Viz threading model — the likely shared root cause.

Tooling added this round (see git history): atomic single-`write()` crash dump in
`base/debug/stack_trace_posix.cc` (R0–R15 + absolute return-address scan),
symbolizable offline with `llvm-addr2line -e out/qnx-arm/exe.unstripped/content_shell <abs-addr>`.

Recommended next steps:

1. Try forcing **in-process cert verification** (disable the out-of-process
   CertVerifierService) at tier 4 to test the shared-root-cause hypothesis.
2. With `--qnx-trace` now crash-safe, capture a clean hang trace and `pidin`
   thread dump; symbolize the parked worker's stack.
3. Inspect CertVerifierService endpoint lifetime/threading
   (`services/cert_verifier/*`, mojo `Connector` teardown) for the race exposed
   by the Viz thread topology.

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
