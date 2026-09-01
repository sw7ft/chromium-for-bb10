# Berry Browser LLVM PGO Runbook

Profile-Guided Optimization for BerryBrowserV3 on QNX ARM (Passport). Expect ~5–10% improvement on native C++ hot paths (Skia, net, compositor); does not fix multi-second JS stalls on heavy SPAs.

## Overview

| Phase | GN `chrome_pgo_phase` | Out dir | Purpose |
|-------|----------------------|---------|---------|
| PGI (collect) | `1` | `out/qnx-arm-pgi` | Instrumented binary → writes `.profraw` on device |
| PGO (ship) | `2` | `out/qnx-arm-pgo` | Optimized binary using merged `.profdata` |

## One-time setup

### 1. Build LLVM profile runtime for QNX ARM

```bash
./deploy/build-qnx-profile-runtime.sh
# -> /root/llvm-qnx-profile/libclang_rt.profile-arm.a
```

Requires LLVM 17.0.6 sources at `/root/llvm-project-17` (auto-downloaded by script).

### 2. GN args

Already checked in:

- [`out/qnx-arm-pgi/args.gn`](../out/qnx-arm-pgi/args.gn) — instrumentation
- [`out/qnx-arm-pgo/args.gn`](../out/qnx-arm-pgo/args.gn) — optimization (needs profdata)

## Collect profiles (device)

```bash
# Build instrumented engine (~full rebuild once)
cd /root/chromium/src
ninja -C out/qnx-arm-pgi content/shell:content_shell

# Deploy to Passport (root SSH, same as deploy-binary.sh)
# Also deploy launcher if berry-pgo-collect marker support needed

./deploy/pgo-collect.sh setup
./deploy/pgo-collect.sh workload   # print checklist
```

Follow the **tapped** workload (GPU/EGL must init). Marker file:

```
/accounts/1000/shared/misc/berry-pgo-collect.enable
```

Sets `LLVM_PROFILE_FILE=/accounts/1000/shared/misc/pgo/berry-%p-%m.profraw` and disables the exit watchdog so profiles flush on close/restart.

## Merge and ship

```bash
./deploy/pgo-collect.sh pull
./deploy/pgo-collect.sh merge
# -> /root/pgo-profiles/berry-qnx-arm.profdata

./deploy/build-pgo-bar.sh
# Builds out/qnx-arm-pgo + packages .bar
```

Or manually:

```bash
ninja -C out/qnx-arm-pgo content/shell:content_shell
BERRY_CONTENT_SHELL=out/qnx-arm-pgo/content_shell ./deploy/build-v3-bar.sh
```

## Validate gains

Enable `berry-fps.enable`, tapped launch, compare before/after in `berry-kbd.log`:

```bash
grep -E 'QNX:BF|QNX:FPS|QNX:GLSWAP' berry-kbd.log
```

| Metric | What to compare |
|--------|-----------------|
| Scroll fps | `QNX:FPS present=` / `QNX:GLSWAP` |
| Main-thread stalls | `QNX:BF` max delta |
| Binary size | `ls -la out/qnx-arm*/content_shell` |

## Regenerate profiles when

- Major engine changes (same git SHA for PGI collect + PGO build is ideal)
- LLVM/clang version bump
- Significant new hot paths (new shims, compositor changes)

Store raw profraw under `/root/pgo-profiles/raw/` (not in git). Record Chromium commit + LLVM version in a note beside `berry-qnx-arm.profdata`.

## Troubleshooting

| Problem | Fix |
|---------|-----|
| Link errors `__llvm_profile_*` undefined | Rebuild `libclang_rt.profile-arm.a`; check `qnx_profile_runtime_path` in args.gn |
| No `.profraw` on device | Confirm `berry-pgo-collect.enable`; close app cleanly (not kill -9); check `shared/misc/pgo/` |
| PGO build warnings (out-of-date) | Normal for minor edits; re-collect if many `-Wno-profile-instr-out-of-date` |
| V8 builtins PGO failure | `v8_enable_builtins_optimization = false` in args (already set) |
| Instrumented binary huge/slow | Expected — never ship PGI build to users |
| PGI link fails on `.profd` in `.so` | Web tests disabled for PGI; ANGLE `.so` skipped — see `content/shell/BUILD.gn`, `ui/gl/BUILD.gn` |
| PGI link bfd SIGSEGV / OOM on monolithic link | 32-bit QNX bfd; try `PGO_LINK_CHUNKS=16 bash deploy/qnx-pgi-staged-link.sh` after compile. If `.profd` COMDAT errors appear, rebuild with `-fmerge-all-constants` disabled (automatic when `chrome_pgo_phase=1` on QNX). |

## Key files

| File | Role |
|------|------|
| [`build/config/compiler/pgo/BUILD.gn`](../build/config/compiler/pgo/BUILD.gn) | QNX PGO flags + profile runtime link |
| [`deploy/build-qnx-profile-runtime.sh`](build-qnx-profile-runtime.sh) | Cross-build profile runtime |
| [`deploy/pgo-collect.sh`](pgo-collect.sh) | Device setup, pull, merge |
| [`deploy/build-pgo-bar.sh`](build-pgo-bar.sh) | PGO build + package |
| [`deploy/qnx-pgi-bin/arm-blackberry-qnx8eabi-ld`](qnx-pgi-bin/arm-blackberry-qnx8eabi-ld) | PGI link wrapper: flatten thin `.a` → QNX bfd |
| [`deploy/qnx-pgi-flatten-archive.sh`](qnx-pgi-flatten-archive.sh) | Thin-archive → fat archive helper |
