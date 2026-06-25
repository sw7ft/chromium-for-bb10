# Caching & Load-Performance Notes (QNX/BB10 content_shell)

Investigation target: make heavy SPAs (esp. WhatsApp Web) load fast enough to be
usable on the Passport. Measured on-device via tapped GPU launches (SSH cannot
init EGL, so WhatsApp stalls pre-login there and is not representative).

## 1. Persistent HTTP disk cache (DONE - commit "Persist HTTP cache to disk")

content_shell left `NetworkContextParams::http_cache_directory` null, which per
`network_context.mojom` yields an in-memory HTTP cache that dies with the
process. Every launch therefore re-downloaded all JS/WASM/static assets.

Fix: `ShellContentBrowserClient::ConfigureNetworkContextParamsForShell` now sets
`http_cache_directory = <profile>/Cache` (QNX-scoped, non-OTR contexts only).
The disk_cache backend is known-good on QNX (GPUCache/DawnCache already persist).

Result (tapped cold vs warm WhatsApp Web): cold filled ~20.3 MB; a warm reload
re-downloaded only ~3.8 MB (~81% served from disk).

## 2. V8 code cache re-enabled (DONE - commit "Re-enable GeneratedCodeCache")

`GetGeneratedCodeCacheSettings` was hard-disabled on QNX over a suspected
FileEnumerator/FilePath-move SIGSEGV during cache-dir enumeration. That no longer
reproduces: verified the simple-cache opendir/readdir enumerator builds and
re-reads `Code Cache/{js,wasm}` cleanly across two launches with zero crashes.

Observed: `Code Cache/js/` populates (first-load timestamp placeholders; full
bytecode lands on the 2nd load). `Code Cache/wasm/` stays EMPTY - WhatsApp's WASM
does not engage Blink streaming code caching
(`v8_wasm_response_extensions.cc`), so the WASM compile cost is NOT cached.
Net: code cache helps JS only.

## 3. V8 flag experiment harness (launcher berry-jsflags marker)

`launcher.c` reads `/accounts/1000/shared/misc/berry-jsflags`; if non-empty it
appends `--js-flags=<contents>` to content_shell. Lets us A/B V8 flags with a
launcher-only rebuild (no content_shell rebuild). Remove the marker to run clean.

Example: `echo "--liftoff-only --wasm-lazy-validation" > .../berry-jsflags`

## 4. The headline load stall is JS/GC-bound, NOT WASM compile

Multi-second `QNX:BF` main-thread stalls dominate WhatsApp load. Attribution:

- `wasm_lazy_compilation` is already default-on, so it is not eager full-module
  compilation.
- With `--trace-wasm-compilation-times` armed, a cold tapped load produced ZERO
  "Compiled function" lines despite downloading `*.wasm` and showing a 34s stall.
- Timeline correlation: 11.8s / 13.9s / 9.4s stalls occurred BEFORE the `.wasm`
  was even received - i.e. pure JavaScript, no WASM in the page yet.
- Confirmatory run with `--liftoff-only --wasm-lazy-validation`: stalls unchanged
  (28.4 / 14.4 / 10.9s vs 34.6 / 13.9 / 11.8s baseline), still zero compile trace.

Conclusion: the stall is JavaScript execution + GC on the 32-bit Krait in
single-process mode - a CPU ceiling, not WASM compilation. WASM compile flags
(`--liftoff-only`, `--wasm-lazy-validation`, tiering-budget) do not help and are
NOT enabled by default. Stalls also vary wildly run-to-run (8-51s), consistent
with runtime/sync/GC work rather than a fixed compile cost.

Remaining (not pursued - low ROI): main-thread scheduling tweaks, GC flags,
out-of-process renderer (historically spins/crashes on QNX). The durable wins are
the two caches above (fewer re-downloads, JS not re-parsed each launch).

## 5. GC probe: ruled out as the stall cause (--trace-gc)

Followed up the JS/GC question with `--trace-gc` (via the berry-jsflags harness)
on a cold tapped WhatsApp load:
- 27 major Mark-Compact GCs, 0 large pauses: longest single GC pause = 318 ms.
- Cumulative main-thread GC time ~4.8 s spread over a ~90 s load.
- Peak heap only ~68 MB (no 32-bit address-space pressure).

The worst `QNX:BF` stall that run was 16.5 s - far larger than any GC pause - so
the big stalls are LONG SYNCHRONOUS JAVASCRIPT TASKS, not GC. Net verdict: the
load stall is raw single-thread JS execution on the 32-bit Krait, a CPU ceiling
no V8 flag removes. Heap/GC tuning is bounded to the ~4.8 s cumulative and is not
worth pursuing.

## 6. Render resolution (perceived-speed lever, not a JS fix)

Already rendering at 720² upscaled to the 1440² panel (QNX_SCREEN_WIDTH/HEIGHT +
QNX_SCREEN_OUTPUT_*). Marker profiles drop further: berry-x-540.enable (540²),
berry-x-420.enable (420²). Lower resolution cuts raster/composite/GPU pixel work
and frees CPU cores that otherwise compete with the JS main thread, improving
responsiveness DURING stalls - but it does not shrink the JS compute itself.
Same for fps caps (berry-x-slow10/12/15) and raster-thread count
(berry-x-1thread/2thread): contention relief, not a cure for the synchronous-JS
ceiling.

## 7. JS-execution levers exhausted - confirmed silicon ceiling

Pushed every remaining "make the JS faster" angle on tapped WhatsApp loads:
- Hardware: `pidin info` shows all 4 QCT Krait cores online at 2265 MHz (the
  Snapdragon 800 rated max) - not downclocked, not core-gated.
- JIT: Sparkplug is compiled in (`v8_enable_sparkplug = !v8_jitless`) and on by
  default, so JS is JIT'd, not interpreted. `--always-sparkplug` (eager baseline)
  left stalls unchanged (15.1 vs 15.3s) - no help.
- Contention: 540² render + 2 raster threads + --no-memory-reducer left the
  worst stall at ~15s (vs ~15-16s) - within run-to-run variance (8-51s).

Verdict: the multi-second load stall is genuine single-threaded JavaScript
compute (WhatsApp's bootstrap/message-processing) at the silicon ceiling of the
32-bit Krait. No V8 flag, process model (multi-process won't parallelize one
synchronous task), or headless mode changes it - headless still runs the same JS
and only skips on-screen present, which is already not the bottleneck.

The durable, real improvements remain: persistent HTTP cache (~81% fewer warm
re-downloads), re-enabled JS code cache (no re-parse each launch), and the
optional 540²/raster/fps profiles for perceived responsiveness during the
unavoidable stall.
