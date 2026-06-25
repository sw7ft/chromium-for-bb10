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
