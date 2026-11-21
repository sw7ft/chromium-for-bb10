# Berry Browser — checkpoint and next plan (Sep 10, 2026)

This is the **production save point**. Build **97** is ThinLTO + on-device PGO,
verified faster than ThinLTO-only build 95. Branch from **this commit** if the
next experiment (multi-process) fails; do not mix MP wreckage into this tree
until it stays up.

| Item | Value |
|------|--------|
| Branch | `berry-v3` |
| Ship binary | `out/qnx-arm-pgo/content_shell` (~83 MB stripped) |
| BAR | `deploy/berry-shell-bar/BerryBrowserV3-3.0.2-build97.bar` |
| App id | `com.sw7ft.BerryShellV3` |
| Device log | `/accounts/1000/shared/misc/berry-kbd.log` |
| Profiles | `/root/pgo-profiles/berry-qnx-arm.profdata` (604k fn, 87B counts) |

Related runbooks: [THINLTO.md](THINLTO.md), [PGO.md](PGO.md). Older product
status (shims, Google, YouTube) is still in [berry-browser-status.md](berry-browser-status.md)
and is **stale on build numbers** — trust this file for engine/perf state.

---

## Progress update — builds 110–112 (Nov 20, 2026)

Current ship build is **112** (`3.0.2.112`), from `out/qnx-arm-pgo-o2`. Three
engine changes since 109, and the first on-device benchmark numbers. The
harness is in `deploy/berrybench`; it runs engines headless over SSH with
software rendering, so its numbers are relative, not what the installed app
scores.

### "97 is the compiler ceiling" was wrong

Every build up to 109 linked with `--lto-O0`. Build 110 links `content_shell`
with `--lto-O2` (see [THINLTO.md](THINLTO.md)): 8% less time on ten DOM-heavy
Speedometer 3.0 suites. The claims below that there is nothing left in the
build configuration predate this.

### Wrong math in cold JavaScript (fixed in 111)

`Math.sin/cos/tan/asin/acos/atan/atan2/exp/expm1/log/log1p/log2/log10/cbrt/
sinh/cosh/tanh/pow`, `%` on non-integers and `**` returned garbage until the
calling function had run often enough to be optimized (`Math.pow(2,10)` gave
2.0000000000002, `5.5 % 2` gave 5.5). `mksnapshot` bakes the C calling
convention for these into the embedded builtins from `USE_EABI_HARDFLOAT`,
which followed gn's `arm_float_abi` (unset, so "hard") while the QNX target
is compiled `-mfloat-abi=softfp`. Fixed in `v8/BUILD.gn`; the whole V8 port is
now exported as `deploy/patches/v8-qnx-bb10.patch`. Present in every earlier
build. `selftest.py` in the harness: 2/26 before, 26/26 after.

`out/qnx-arm-pgo` (the O0 fallback) has not been rebuilt since; it picks the
fix up on its next build.

### Late load event on a re-pointed iframe (fixed in 112)

The port posts `Document::FinishImplicitClose` instead of running it inline,
so the load event of an iframe's initial `about:blank` arrived after script
had already set `onload` and `src`, and the handler ran against the blank
document. Speedometer 3.0 could not start a suite. The stale event is now
dropped. `iframetest.py`: 4/7 before, 7/7 after. Unmodified Speedometer 3.0
runs all 20 suites.

### `--no-optimize-for-size`

Low-end device mode forces V8's `--optimize-for-size`. Turning it off through
the `berry-jsflags` marker (`--max-old-space-size=256
--no-optimize-for-size`) is the largest single gain measured: 23% less time
on the ten suites on top of the O2 link, 17% on all 20. On swiftapp.ca the
four startup stalls went from 24.4 s (build 106) to 15.4 s (build 112 with
the marker), with the JS heap peaking at 120 MB instead of 64 MB. **Not yet
the launcher default** — next build.

### Baseline for the next comparison (build 112 with the marker, Nov 20)

| Site | Stalls over 1.5 s | Total | Longest |
|---|---|---|---|
| m.facebook.com | 3 | 5.9 s | 2.7 s |
| messenger.com (login to first chat) | 10 | 29.5 s | 5.5 s |
| amazon.ca (home, Black Friday, search, product) | 7 | 26.6 s | 7.3 s |
| swiftapp.ca (cold start) | 4 | 15.4 s | 5.3 s |

Messenger and Facebook log in and work on 112. GC is 3–10% of the remaining
stall time; the rest is script and layout and has not been profiled yet.

### Open

- Profile what the main thread does during the remaining stalls.
- swiftapp.ca throws `Set.difference is not a function`. V8 12.0 has the Set
  methods staged; `--harmony-set-methods` enables them (checked on the bench
  engine).
- The iframe fix drops the stale event rather than restoring upstream's
  synchronous timing. An iframe that is inserted, given a handler afterwards
  and never navigated still gets one event that Chrome would not deliver.

---

## Progress update — builds 98–106 (Sep 13, 2026)

Current ship build is **106** (`3.0.2.106`), same PGO+ThinLTO binary lineage as
97. Engine build config is unchanged; everything below is runtime or UI.

### Multi-process is no longer blocked

The reason MP was "unproven" is found and fixed. On QNX, `sendmsg()` carrying
`SCM_RIGHTS` **silently truncates** the payload past a threshold, so large
Mojo messages (notably `CommitNavigation`) desynchronised the channel and the
renderer died with a white screen and garbled message headers. Diagnosed with
a standalone `deploy/qnx-socket-probe.c`; fixed by clamping `iov_len` to 2048
bytes on handle-carrying sends. MP now runs, exposed as a Settings toggle,
**default off** pending a long soak.

This matters beyond speed: single-process means a renderer JS stall freezes
chrome, input and compositor together, which is exactly the Facebook symptom
below. MP contains it.

### Toolbar / swipe-reveal experiment: tried, reverted

Build 100 added a swipe-down reveal for the URL bar; build 101 reverted the
whole thing back to build 98 behaviour. Recorded so it is not re-attempted
blind: the toolbar survives in the Screen window buffer only because the inset
web content never draws over those rows. A bar floating above full-window
content is overwritten by the next page frame, and keeping it alive needs a
strip re-post every frame — which is what made `RepaintToolbar` flicker badly
enough to be disabled originally. A GL-composited version was also tried and
withdrawn. Preserved at tag `berry-toolbar-experiment-backup`.

### Facebook login freeze — open, root cause not yet confirmed

Symptom: page paints its icons, then the renderer main thread freezes. Not a
crash (renderer survives) and not the account gate (no error page). Two fixes
shipped, neither confirmed, because the device log has been unreachable:

- **Telemetry blocklist was not fast-failing.** It mapped `pixel.facebook.com`
  and friends to `0.0.0.0`, which *resolves successfully* and then blocks on a
  TCP connect to `0.0.0.0` until timeout — the opposite of the stated intent.
  Now `^NOTFOUND`, which fails instantly with `ERR_NAME_NOT_RESOLVED` and no
  socket. See `net/dns/mapped_host_resolver.cc`.
- **Desktop UA on all of `www.facebook.com`** (build 66, needed for the
  Messenger checkpoint flow) drops login into the full desktop bundle. Left as
  default; `berry-fb-mobile.enable` opts out for A/B.

**Likely regression window:** build 67 removed two stale markers left on the
device since Jul 10 debugging, `berry-quic.disable` and `berry-block.disable`.
The telemetry blocklist had therefore never actually run while Facebook worked,
and went live at exactly the build where Facebook broke. Turning **Block
telemetry** off reproduces the pre-67 state and is the first thing to try.

### New diagnostics and knobs (all Settings-exposed, no SSH needed)

| Marker | Settings | Purpose |
|---|---|---|
| `berry-heap-mb` | JS memory | V8 old-space cap, 256/384/512/768/1024 picker |
| `berry-fb-mobile.enable` | Performance | Mobile UA for `www.facebook.com` |
| `berry-fps.enable` | Developer | Per-second frame/pump timing; catches stalls that miss the main-thread heartbeat |

### Perf audit (Sep 13) — checked, for the record

- `is_official_build` is **not** a missed win. `default_optimization` resolves
  to `:optimize` for any non-debug build and `optimize_for_size` is false on
  QNX, so we already get `-O2`. The "97 is the compiler ceiling" call stands.
- Most low-end-mode GPU penalties are `IS_ANDROID`-gated and never fire here.
  One is not: `ImageDecodeCacheUtils` forces the low-end decoded-image working
  set despite the Passport's 3GB, so images re-decode on scroll. Testable with
  the existing **Low-end mode** toggle.
- `init_stack_vars` defaults true for QNX, adding
  `-ftrivial-auto-var-init=pattern` everywhere. Setting it false is a real but
  unmeasured win; costs a full ThinLTO+PGO rebuild, and removes a
  determinism aid we have leaned on while chasing SIGSEGVs.

### Blocked on

Device log. The USB bridge (`host.docker.internal:2222` → `169.254.0.1:22`)
has been down all session, so `QNX:MT stall=...-> GC | non-GC(JS/paint)` — the
line that decides GC tuning vs bundle size — is unread. `berry-kbd.log` lives
on the shared partition and can be pulled over USB mass storage without SSH.

---

## What is done (do not re-litigate)

- **ThinLTO** (build 95): clang-17 bitcode + toolchain `ld` → `ld.lld-17`.
  Fixes: emulated TLS at LTO, `QnxThreadFuncAligned` used/retain, stack
  canary as a **pointer** in `qnx_platform_stubs.cc` (build 94 boot crash).
- **PGO** (builds 96→97): phase-1 instrumented ThinLTO linked; phase-2 used
  a real Passport browsing profile. User-visible win vs 95 on native paths
  (event loop, V8 GC, CSS, compositor). Does **not** erase first-load SPA
  JS compile stalls (X hydrating).
- **Stability that got us here:** Skia `ButtCapDashedCircleOp` skipped on
  QNX (Adreno shader compiler SIGSEGV); WebAudio/PFFFT/PeriodicWave fail
  safe (x.com fingerprint / WebRTC); stall sampler + hardened crash handler
  (`QNX:LIBMAP`, regs, `msync` stack scan); boot log-ship (opt-out).
- **PGO landmines (already fixed, see PGO.md):** no `%m` merge with the 60s
  flusher (u64 wrap); skip counter-less `__profd_` stubs in compiler-rt
  registration; `no_profile_instrument_function` on the naked QNX thread
  trampoline; `-fno-split-lto-unit` + `-disable-vp` on PGI.

Rebuild 97:

```bash
# needs /root/pgo-profiles/berry-qnx-arm.profdata
ninja -C out/qnx-arm-pgo content_shell
BERRY_CONTENT_SHELL=$PWD/out/qnx-arm-pgo/content_shell deploy/build-v3-bar.sh
```

Push without a full `.bar` (root SSH, LAN IP changes):

```bash
# Host alias pattern: root@<ip>  key ~/.ssh/id_rsa4x
# Install dir: /apps/com.sw7ft.BerryShellV3.testRel_erryShellV3d55e24f1/native/
```

---

## Next: multi-process (only experiment that can change how stalls *feel*)

Single-process means a 2–6s renderer JS stall freezes chrome, input, and
compositor. MP keeps the browser process alive while the renderer thinks.
GPU stays in-process (`--in-process-gpu`) — do not split GL on Adreno 225/330.

### How to try (no rebuild)

1. Fully close Berry Browser (not just background).
2. On device: `touch /accounts/1000/shared/misc/berry-mp.enable`  
   or SSH: `QNX_MULTI_PROCESS=1` (launcher also reads the marker).
3. Tap the tile. Log must say `multi-process mode` (launcher.c).
4. Load X, YouTube watch, a couple other sites. Watch for:
   - chrome still scrolling/accepting keys while the page is busy
   - renderer crash that does **not** kill the browser
   - boot hang / second `content_shell.exe` failing to spawn
   - GPU/viz init dying (in-process GPU should look like 97)

Disable: remove the marker, relaunch. You are back on this checkpoint’s
behavior.

### If it fails

1. Delete `berry-mp.enable`, confirm 97 is the running binary again.
2. **Branch from this commit** (`berry-v3` at the ROADMAP commit) for any
   MP-only patches. Do not land speculative process/sandbox changes on
   the ship branch until a session stays up.
3. Capture `berry-kbd.log` (`QNX:CRASH`, `QNX:LIBMAP`, launcher spawn lines).

Known prior state: marker was armed once; device dropped before a clean
result. Treat MP as unproven.

---

## After MP (or if you skip it)

| Priority | Work | Why / why not |
|----------|------|----------------|
| High | Confirm V8 code cache hits on repeat X/YouTube | First visit will still hitch; repeats should not. Log / trace cache before new V8 flags. |
| Medium | Another PGO session **only** if the site mix changes a lot | Same sites = diminishing returns. Collect with `%p` snapshots, merge offline. |
| Low | V8 lazy-parse / compile-budget flags | Easy to regress; measure with `QNX:MT stall=` lines first. |
| Don’t | Cloudflare Turnstile / WebGL2 | Adreno 225 has no GLES3; not a software bug. |
| Don’t | More ThinLTO/PGO rebuilds “just because” | 97 is the compiler ceiling for this profile. |
| Don’t | Fight YouTube home/kevlar or Google `/sorry` checkbox | Route-around strategy in berry-browser-status.md still applies. |

---

## Out dirs (do not confuse)

| Dir | Phase | Ship? |
|-----|-------|-------|
| `out/qnx-arm-lto` | ThinLTO only (build 95) | fallback if PGO binary is suspect |
| `out/qnx-arm-pgilto` | PGI instrumented (build 96) | profiling only; sluggish |
| `out/qnx-arm-pgo` | PGO + ThinLTO, `--lto-O0` link (builds 97–109) | fallback |
| `out/qnx-arm-pgo-o2` | **PGO + ThinLTO, `--lto-O2` link (build 110+)** | **yes** |

---

## Suggested git move for the MP attempt

```bash
git branch berry-v3-mp-experiment   # from this checkpoint
git switch berry-v3-mp-experiment
# try marker-only first; commit code only if spawn/lifetime needs patches
# if it dies: git switch berry-v3   # ship 97 unchanged
```
