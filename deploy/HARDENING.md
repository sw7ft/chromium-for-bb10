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

### Vector B is now DETERMINISTIC via CDP (Jun 17 2026) — gates the browser plan

Implementing Phase 1 of the CDP-screencast browser plan turned the ~20% flake
into a **100%, minimal, deterministic repro** that localizes the fault to the
**renderer**, with the browser process fully healthy.

Transport finding (works): Chromium's port-based DevTools (`net::HttpServer`)
is **broken on QNX** — it rejects HTTP/1.0 (`http_server.cc(454)`) and **hangs
with no response on HTTP/1.1** (the WS upgrade and `/json` never reply; tunnel
connects then RST/timeout). The working transport is **`--remote-debugging-pipe`**
(CDP over fd3 in / fd4 out, `\0`-delimited JSON; `read_fd=3`, `write_fd=4`).
Node v22 is bundled on-device (`./node/node`) and drives the pipe natively via
`stdio:[…,'pipe','pipe']` (Puppeteer's pipe transport). Probe:
`deploy/cdp-pipe-probe.js` (host helper `deploy/cdp-probe.js` / `cdp-probe.py`).

What works over the pipe (browser process):
- `Target.getTargets`, `Target.setAutoAttach{flatten:true}` → a `page` session
  `sessionId` is delivered (event can arrive *before* the setAutoAttach ack —
  capture it eagerly).

What hangs — **every** renderer-touching command, single- AND multi-process:
- `Page.captureScreenshot` (even of `about:blank`; `--run-all-compositor-stages-
  before-draw` does not help) — needs a renderer-submitted CompositorFrame that
  never arrives.
- `Runtime.evaluate '1+1'` — renderer main-thread JS never runs the inbound msg.
- `Page.printToPDF` — compositor-free Blink paint also hangs.
- `Page.enable` (renderer half) — never acks.

Conclusion: once the renderer goes **idle** after startup, browser→renderer work
(tasks/mojo/timers) is **not delivered/woken** — so the renderer compositor never
produces a frame and the DevTools agent never runs. The browser side is fine
(browser-level CDP responds instantly). This is the same lost-wakeup as Vector B,
now reproducible on demand. Note `dump-dom` "works ~75%" only because that path
drives the renderer immediately at startup and needs **no CompositorFrame**;
it does not prove the renderer compositor/idle-wakeup works.

Already-present QNX wakeup patches (so these are *not* the remaining bug):
- `base/message_loop/message_pump_libevent.cc`: IO-thread wakeup uses a
  `socketpair` (not `pipe`) because QNX `select()` only reports readability on
  socket FDs; `OnWakeup` drains all bytes.
- `mojo/core/channel_posix.cc`: socket-based channel, `IOV_MAX` define, QNX trace.

Implication for the plan: **Phase 1 = NO-GO** for the naive design. The CDP
screencast/print/screenshot paths all require the renderer to produce
output, which is exactly what is wedged. Warm-daemon-on-`about:blank` makes it
*worse* (100% vs ~25%) because it depends on waking an idle renderer. The
prerequisite "build tweak" the plan anticipated is **root-causing the renderer
idle-wakeup / browser→renderer delivery stall** — this gates every rendering
approach (headless OR on-screen), not just CDP.

Decisive next experiment: in multi-process with `QNX_TRACE`/`--qnx-trace`, send
`Runtime.evaluate` and check the **renderer** process for `QNX:Mojo:Read` —
if absent, the renderer IO thread's socket watch isn't notified (libevent/select
on the channel FD); if present, the IO→main `WaitableEvent` wakeup is lost.

Further evidence gathered (Jun 17 2026, same session):
- **Not slowness — a true stall.** `Runtime.evaluate '1+1'` to the page session
  does not return even with a **180s** timeout (no tracing). 3 min is far beyond
  any init time on this device.
- **No DevTools agent ever engages.** A full single-process `QNX_TRACE` capture
  of an `eval` stall (`/tmp/engine.log`, ~10k lines) shows **zero**
  `blink.mojom.DevToolsAgent`/`DevToolsSession` mojo activity. blink frame
  interfaces *do* bind (`Widget`, `FrameWidget`, `PageBroadcast`,
  `LocalFrameHost`, `BrowserInterfaceBroker`, `AssociatedInterfaceProvider`), so
  the frame partly comes up, but the renderer DevTools agent never binds/runs.
- **Tracing is too slow to reach steady state.** With `QNX_TRACE=1` the device is
  so throttled by per-task `write(2)` that at 60s it is *still* running browser
  init tasks (blob storage, disk-cache `SyncInit`, accessibility, audio monitor).
  So in-trace "hangs at ICU" were measurement artifacts (SSH/stderr backpressure
  + slowdown), not the bug. Drive trace output into a drained file
  (`ENGINE_LOG=`) and use long `ATTACH_TIMEOUT`/`CALL_TIMEOUT`.
- **Primitives audited, look correct.** `condition_variable_posix.cc` uses
  `CLOCK_MONOTONIC` (constructor + `clock_gettime`); the idle wait is an infinite
  `pthread_cond_wait` (clock-independent), so a QNX monotonic-clock quirk would
  cause busy-spin, not this hang. `waitable_event_posix.cc` is the standard
  lock+CV wait-list. The IO-thread wakeup pipe is already a `socketpair`.
- Net (SUPERSEDED below): earlier reading was a renderer lost-wakeup. Dynamic
  instrumentation (Jun 17 2026 PM) disproves that — see UPDATE.

### UPDATE (Jun 17 2026 PM) — root cause re-localized to the BROWSER UI thread

Method: runtime trace gate (`SIGUSR1` flips `g_qnx_trace_enabled` in
`shell_main.cc` — boot untraced, enable just before the probe action), plus
per-pump trace in `MessagePumpDefault` (`QNX:MPD:*`) and `MessagePumpLibevent`
(`QNX:MPL:*`), plus all-thread `/proc` register/stack reads via `qnx_stack`
on the held (`HOLD=1 HOLD_CMD=eval`) process, symbolized offline against
`out/qnx-arm/exe.unstripped/content_shell` (non-PIE EXEC, text
`0x826e80..0x5811e10`).

Findings (deterministic, single-process `MODE=eval` of `1+1` on `about:blank`):
- **The renderer is healthy.** Trace shows the renderer commits about:blank
  (`RFI:CommitNav`/`DidCommitNav`/`DidFinishLoad`) AND **receives and dispatches
  the DevTools command** — `QNX:IEC` Accept of `blink.mojom.DevToolsAgent` then
  `blink.mojom.DevToolsSession` both return `res=1` on the renderer main thread.
  The earlier "no DevTools agent activity" was a trace-timing artifact of booting
  under heavy trace; with the SIGUSR1 gate the agent clearly engages.
- **The browser UI thread deadlocks during navigation commit.** All threads were
  sampled twice 1s apart: tid=1 (browser main), the renderer main, and the
  storage thread all have **identical pc/lr/sp** across samples → truly blocked,
  not spinning. tid=1's symbolized stack scan is the commit path:
  `mojo::Connector::OnWatcherHandleReady` → `RenderFrameHostImpl::
  DidCommitNavigationInternal` → `PageFactory::Create`/`PageImpl::PageImpl` →
  `Navigator::DidNavigate` → `WebContentsImpl::DidFinishNavigation` →
  `NotifyObservers` → `PerformanceManagerTabHelper::DidFinishNavigation` →
  `PerformanceManagerImpl::CallOnGraphImpl` → `PostTask`/`PostDelayedTask`, then
  a libc `pthread_cond_wait`. So browser-main blocks on a **synchronous
  primitive while still inside `DidCommitNavigation`** and never returns to its
  message loop.
- **That is why there is no renderer output.** The renderer produces/returns the
  eval result, but browser-main never runs its loop again, so the response is
  never forwarded to the `--remote-debugging-pipe` writer → CDP call times out.
  Same mechanism blocks every renderer round-trip (screenshot/screencast/PDF),
  matching the 100% deterministic "renderer never produces output" symptom.
- **tid=1 is NOT parked in either message pump.** No `QNX:MPD:Wait tid=1` and no
  `QNX:MPL:Wait tid=1` anywhere in 10k trace lines; its blocked pc is the condvar
  primitive (distinct from the libevent IO threads which block in `select`). So
  this is a real synchronous deadlock, not a lost pump wakeup.
- **leveldb LOCK failures are a red herring.** `ChromiumEnv::LockFile`
  (`storage::DomStorageDatabase` / `shared_proto_db`) fails to take the lock on
  QNX (the spammed `...LOCK: No further details (LockFile::1)` warnings), retries
  for only ~1s (`kMaxRetryDuration`) then returns an error and the thread goes
  idle. Reproduces with a fresh unique `--user-data-dir`, so it is not a stale
  lock and not the deadlock — but `FilesystemProxy::LockFile` on QNX is worth a
  separate fix (QNX fs likely lacks the fcntl/flock semantics leveldb expects).

### UPDATE 2 (Jun 17 2026, later) — primitive isolation via instrumentation

Added always-on (ungated) QNX traces around each candidate blocking primitive and
re-ran `MODE=eval`, plus read live thread states with `pidin -p <pid>`:
- **Mojo `[Sync]` calls — ruled out.** Traced `InterfaceEndpointClient`'s
  `SyncWatch`/`SyncWatchExclusive` (`QNX:SYNC:wait/done`). **Zero** sync calls in
  the whole run → tid=1 is not in a mojo sync call.
- **`base::WaitableEvent::Wait()` — ruled out.** Traced entry/exit for non-idle
  waits (`QNX:WE:wait/ret`, gated on `!only_used_while_idle_`). All 16 matched; no
  unmatched wait on tid=1.
  (Note: `__builtin_return_address(1)` crashes under `-fomit-frame-pointer` on this
  toolchain — it turned the hang into a deterministic SIGSEGV. Use only
  `__builtin_return_address(0)`.)
- **`base::ConditionVariable::Wait()` — ruled out for tid=1.** Traced entry/exit
  (`QNX:CV:wait/ret`). The only unmatched waits are the expected idle parks:
  16× `WaitableEvent::TimedWaitImpl` (pump/threadpool idle) + 1×
  `cc::SingleThreadTaskGraphRunner::Run` (raster idle). **tid=1 emits no CV line
  at all.**
- **`pidin` thread states (28 threads, all parked):** tid=1 = `CONDVAR (0x7d635fc)`
  — and that condvar object is **not** on tid=1's stack (idle SyncWaiter condvars
  sit at `0x7ffxxxxx` stack addresses). So tid=1 blocks on a **heap/member condvar
  that is NOT a `base::ConditionVariable`/`WaitableEvent`** — i.e. a raw
  `pthread_cond_t` / `std::condition_variable` owned by some subsystem. tid=1's
  `lr` is inside libc (the wait is invoked by a library function, not directly by
  content_shell code), consistent with `std::condition_variable`. All other
  threads are benign (`CONDVAR` idle, `SIGWAITINFO`, one `REPLY` = the DevTools
  pipe reader doing a blocking fd read).

Net: browser UI thread deterministically parks in a **non-base condvar during
`DidCommitNavigation`** (stack: `DidCommitNavigationInternal` → `PageImpl` ctor →
`Navigator::DidNavigate` → `WebContentsImpl::DidFinishNavigation` →
`PerformanceManagerTabHelper::DidFinishNavigation` → `CallOnGraphImpl` →
`PostTask`/`PostDelayedTask`). Renderer is healthy and Accepts the eval; the
response is never delivered because browser-main never returns to its loop.

Remaining unknown: the exact owner of condvar `0x7d635fc`. The qnx_stack scan
can't order frames (clang release omits frame pointers), so the top frames may be
stale. To finish:
- (a) **clean unwind**: build with `-fno-omit-frame-pointer` (or teach `qnx_stack`
  to use `.eh_frame`) and re-read tid=1 — names the exact caller in one shot; or
- (b) **bisect** commit-time subsystems likely to use a raw/std condvar: in-process
  Viz/GPU SharedImage path (`gpu::ClientSharedImageInterface::CreateSharedImage`
  appears deeper in tid=1's stack; in-process GPU is degraded — "Vulkan not
  supported with in process gpu"), then PerformanceManager. Disable each and
  re-run `MODE=eval`.

Instrumentation seam reference (all `#if BUILDFLAG(IS_QNX)`):
`shell_main.cc` SIGUSR1 trace gate + on-demand `QnxDumpAllThreadStacks()`;
`message_pump_default.cc` `QNX:MPD:*`; `message_pump_libevent.cc` `QNX:MPL:*`;
`interface_endpoint_client.cc` `QNX:SYNC:*`; `waitable_event.cc` `QNX:WE:*`;
`condition_variable_posix.cc` `QNX:CV:*`; `stack_trace_posix.cc` SIGUSR2 sampler
(`QNX:SAMPLE ... UNW:` real-unwind attempt); `qnx_hard_watchdog.cc`
`QnxDumpAllThreadStacks()` (pthread_kill(SIGUSR2) broadcast, reaches tid=1).

### UPDATE 3 — all in-process unwinders fail on ARM32-Thumb/QNX

Three independent backtrace mechanisms were tried to name tid=1's exact blocking
frame; **all fail on this target**, so the stale top-frames from the scan can NOT
be trusted:
1. **Frame pointers** — `enable_frame_pointers` defaults false for `current_cpu
   == "arm"` and `can_unwind_with_frame_pointers` is forced false for ARM+Thumb
   (compiler.gni; clang LLVM bug 18505). A frame-pointer build would not produce
   unwindable stacks here.
2. **Raw stack scan** (`qnx_stack` / SIGUSR2 `ABS:`) — lists every stack word that
   resolves into the module, but cannot order them; tops are demonstrably stale
   (e.g. it showed `CallOnGraphImpl`→`PostTask`, but that path is a pure async
   post that cannot block — confirmed by reading `performance_manager_impl.cc`).
3. **`_Unwind_Backtrace`** (`.ARM.exidx`/CFI) from the SIGUSR2 handler — spins on
   a single IP (48× identical) and never advances: QNX's signal trampoline / the
   in-handler PC has no traversable CFI, so the libgcc unwinder cannot compute the
   caller.

Net: WHERE is nailed (browser-main, non-base heap condvar, during the
`DidCommitNavigation` window, both single- and multi-process), but WHAT (the exact
owning subsystem) cannot be obtained by stack unwinding on this platform.

Only viable path left to the exact culprit: **semantic bisect** — add always-on
`ENTER`/`EXIT` traces (like the `QNX:*` seams above) around browser-UI commit
functions and narrow by which one logs ENTER but never EXIT. Start at
`RenderFrameHostImpl::DidCommitNavigationInternal` and walk into its children
(compositor/GPU frame-sink setup, `PageImpl` ctor, `Navigator::DidNavigate`).
Each step is a content-lib rebuild (~3-4 min).

Working harness produced this session (reusable for the fix):
- `deploy/cdp-pipe-probe.js` — Node pipe-CDP client (spawns content_shell with
  `--remote-debugging-pipe`, fd3/fd4, `\0` JSON). Envs: `MODE=eval|pdf|bcmd`,
  `NONAV=1`, `HOLD=1 HOLD_CMD=eval|shot|none`, `NOATTACH=1`, `NOSINGLE=1`,
  `EXTRA="<flags>"`, `QNX_TRACE=1`, `ENGINE_LOG=<path>`, `ATTACH_TIMEOUT`,
  `CALL_TIMEOUT`.

### UPDATE 4 (Jun 17 2026) — ROOT CAUSE FOUND & FIXED: recursive static-init in UKM

The commit freeze is **fixed**. `Runtime.evaluate`, `Browser.getVersion`,
`Target.getTargets`, navigation and DOM all work over CDP now.

How it was found (no working stack unwinder needed):
1. `MODE=bcmd` (new) proved the wedge is **not** triggered by the eval/screenshot
   command: even `Browser.getVersion` hangs, and it hangs with `NOATTACH=1` (no CDP
   sent at all) — so the **browser UI thread wedges a few seconds into interactive
   startup**, independent of any command. `--dump-dom` (reader) dodges it by exiting
   first. Disabling `Viz` / `NetworkServiceDedicatedThread` / the tier-0 feature set
   did **not** fix it (only shifted timing).
2. Existing `QNX:RT`/`IEC` task traces (no rebuild) showed tid=1's last task is a
   mojo `Accept` during the about:blank commit, after which it parks on a **non-base
   condvar** (base `WaitableEvent`/`ConditionVariable`/`WaitMany`/mojo `[Sync]` all
   already instrumented and ruled out).
3. Global symbol interposition of `pthread_cond_wait` (a `--wrap` linker hook only
   catches executable callers and **missed** it — the wait is made from a shared
   lib) named the caller: `libstdc++.so.6 + 0x7a339` = **`__cxa_guard_acquire`**.
4. Interposing `__cxa_guard_acquire`/`release` named the exact static: the guard
   for the function-local `static`s in `ukm::SourceIdObj::FromOtherId`
   (`services/metrics/public/cpp/ukm_source_id.cc`), inlined into
   `ukm::ConvertToSourceId`. The trace `GA:enter g=X → GA:exit ret=1 → GA:enter g=X
   (stuck)`, with **no other thread touching guard X**, is a **same-thread
   re-entrant** guard acquire.

Why it hangs on QNX specifically: QNX libstdc++ is built **without futex**, so
`__cxa_guard_acquire` serializes ALL static-init guards on a single global mutex +
condvar and does **not** detect same-thread recursion (no `recursive_init_error`);
the thread waits on its own in-progress guard forever. (On Linux/futex builds the
same code self-detects and throws, so this never manifested upstream.)

Fix (`services/metrics/public/cpp/ukm_source_id.cc`): replace the lazy
function-local `static const kNumTypeBits = GetNumTypeBits()` /
`kTypeMask` in `FromOtherId`/`GetType` with namespace-scope **`constexpr`**
constants (`kNumTypeBits = ceil(log2(kMaxValue+1))` computed at compile time).
This removes the runtime guard entirely; behaviour is identical on all platforms.
Validated on-device with a clean binary (no diagnostic scaffolding): eval returns,
browser-side commands return, navigation works.

REMAINING (separate, was masked behind the freeze): `Page.captureScreenshot`
still hangs, but the browser is now **quiescent and healthy** (tid=1 = SIGWAITINFO
idle, all threads idle, no deadlock) — it is waiting on a **compositor frame that
is never produced** in headless/software mode on QNX. This is the frame-production
/ "renderer-output" problem, not a deadlock, and is the next item for the
interactive (screencast) browser. The reader/DOM path remains fully working.
- `deploy/cdp-probe.js` / `deploy/cdp-probe.py` — host/device WS clients (kept for
  reference; the WS/HTTP DevTools server is unusable on QNX).

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
