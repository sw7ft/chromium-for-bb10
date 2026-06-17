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
| **3** | FLAKY | FLAKY | FLAKY | +NetworkServiceDedicatedThread (single-process); ~25% baseline startup stall (9/12), masked by tiny earlier sample. See Vector B. |
| 4 | FLAKY | FLAKY | FLAKY | +Viz (single-process); cert mojo race **FIXED** (11/15, was ~40%); residual ~= tier-3 baseline startup stall (not Viz). Boot watchdog contains failures. See below. |
| 5 | - | - | - | +ServiceWorker (single-process); blocked on tier 4 |
| 6 | - | - | - | Multi-process; `posix_spawn` launcher wired; blocked on tier 4 |
| 7 | - | - | - | qnx_screen on-screen (software); pending |
| 8 | - | - | - | Full GPU; pending |

Tier 1+ keeps `--ignore-certificate-errors`. Tier 1+ drops `--disable-http2` when `cacert.pem` is present.

### Tier 4 (Viz) — investigation findings (Jun 16 2026)

Tier 4 is **not** a hard deadlock; it is a **non-deterministic race**. The boot
watchdog (armed at process start) contains every failure mode, so the device
never wedges. We found **two independent failure vectors**; one is now fixed.

**Vector A — CertVerifierService mojo race (FIXED).**
Every HTTPS load triggered the out-of-process `cert_verifier.mojom.CertVerifierService`
mojo path. On QNX there is **no system trust store**, so the builtin path builder
fails every chain (`CertVerifyProcBuiltin ... failed: No matching issuer found`,
no AIA fetcher) and then must hand the verdict back across the
network-service↔cert-verifier mojo pipe. Under the Viz thread topology that
cross-thread reply raced:
- *Crash (tracing-only):* SIGSEGV in QNX libc `_Putfld`/`strlen` (`R0=0x1f`) when
  `QNX_TRACE_FMT(... iface=%s ...)` printed a torn `Connector::interface_name_`.
  Fixed earlier with `base::QnxSafeStr()`.
- *Hang:* a cert worker parked forever on a heap `ConditionVariable` waiting for
  the verdict to be delivered; the page never progressed past the TLS handshake.

**Fix:** since bring-up always passes `--ignore-certificate-errors`, the full
verify + mojo round-trip is pure overhead that also races. `IgnoreErrorsCertVerifier`
already short-circuits to `net::OK` *before* calling the wrapped (mojo) verifier
for allowlisted SPKIs — but only when `--ignore-certificate-errors-spki-list` is
set. We extended `MaybeWrapCertVerifier` so the plain `--ignore-certificate-errors`
switch engages an **ignore-all** mode that short-circuits every chain. This
bypasses the `MojoCertVerifier` round-trip entirely (verifier stack is
`IgnoreErrors → Caching → Coalescing → CertAndCT → MojoCertVerifier`), so the
racy pipe is never exercised. Bonus: faster HTTPS on **all** tiers (no per-request
path building). Confirmed: `CertVerifyProcBuiltin` log gone (0/15 runs), tier-4
success 11/15 (was ~40%).

**Vector B — quiescent startup stall (OPEN, ~20-25%, BASELINE not Viz).**
This is **not** Viz-specific. Tier 3 (Viz disabled) run ~12-20x shows the
**same** ~20-25% failure (hang or empty/partial dump); the earlier "tier 3
3/3 stable" was too small a sample. Tier 4's extra instability was almost
entirely Vector A; with that fixed, tier 4 (11/15) ~= tier 3 (9/12) baseline.
The `gpu_init.cc: Vulkan not supported with in process gpu` log is a red
herring: it prints at tier 3 too (an in-process GPU thread always starts) and
is just a LOG line, not the hang.

**Full /proc all-thread dump of a live hang (all 26 threads) shows every thread
idle in a normal wait** — no blocked owner, no GPU-init frame:
- tid 1 browser main: `BrowserMainLoop::RunMainMessageLoop -> RunLoop::Run ->
  MessagePumpLibevent -> event_base_loop -> select_dispatch` (idle, pumping).
- IO threads (`BrowserProcessIOThread`, `ChildIOThread`), the
  `NetworkServiceDedicatedThread`, the HTTP cache thread, the in-process
  renderer/blink threads, `cc::SingleThreadTaskGraphRunner`, and all
  `ThreadPool` workers: idle in `MessagePumpDefault::Run` / `WaitForWork` /
  `ConditionVariable::Wait`.

So the failure is a **lost-wakeup / lost-task quiescent deadlock** in the
startup->first-navigation handoff: the browser comes fully up (`DevTools
listening`) but the initial navigation never proceeds and no thread has work.
No active network request, no renderer work — the triggering task/mojo message
for the first load appears to be dropped under a QNX startup race.

Tooling added (committed):
- atomic single-`write()` crash dump in `base/debug/stack_trace_posix.cc`
  (R0-R15 + absolute return-address scan), symbolizable offline with
  `llvm-addr2line -e out/qnx-arm/exe.unstripped/content_shell <abs-addr>`.
- all-thread SIGUSR2 sampler on watchdog deadline (caveat: signal-masked
  threads don't respond — superseded for hangs by the /proc reader below).
- **`qnx_stack.c` + `deploy/capture-hang.sh`**: a `/proc/<pid>/as` thread
  register+stack reader that works on *every* thread regardless of signal
  mask. `capture-hang.sh` launches content_shell, waits past the healthy
  finish time, and on a hang dumps all tids to `/tmp/stacks.txt`. This is the
  decisive tool and produced the all-idle finding above.

Recommended next steps (Vector B):

1. Trace the startup->first-load task flow on the UI thread (Shell window
   creation + `Shell::LoadURL` / `NavigationController::LoadURL` post) to find
   which expected task/mojo message is lost in the ~20% hang. Stacks are
   exhausted (all idle); this needs event/task tracing, not stack dumps.
2. Suspect a `PostTask`/mojo message racing with message-loop or interface
   bring-up that occasionally drops its wakeup on QNX.
3. The fault predates Viz, so it gates *every* tier — worth fixing before
   pushing further up the ladder.

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
