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
| `out/qnx-arm-pgo` | **PGO + ThinLTO (build 97)** | **yes** |

---

## Suggested git move for the MP attempt

```bash
git branch berry-v3-mp-experiment   # from this checkpoint
git switch berry-v3-mp-experiment
# try marker-only first; commit code only if spawn/lifetime needs patches
# if it dies: git switch berry-v3   # ship 97 unchanged
```
