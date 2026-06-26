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

## 8. "Desktop-class -> mobile-class" engine reconfiguration (Snapdragon 801)

The "silicon ceiling" in section 7 was measured against the DESKTOP WhatsApp
bundle in a Chromium configured as desktop Linux. That is the heaviest mode. The
Passport SoC (Snapdragon 801, quad Krait 400 @ 2265 MHz, Adreno 330, 3 GB) is
the class Chrome's mobile path targets. Reconfiguring toward mobile moves the
ceiling by reducing the amount of work, not by speeding the CPU.

Changes (all on QNX):
- Mobile-first identity. Network-context default UA = Android Chrome
  ("Mozilla/5.0 (Linux; Android 10; K) ... Chrome/120 Mobile Safari/537.36"),
  Sec-CH-UA-Mobile ?1, platform=Android. Sites serve their lighter mobile
  bundles => less bootstrap JS. See shell_content_browser_client.cc
  GetBerryUserAgent()/GetQnxChromeUserAgentMetadata(mobile).
  NOTE: content::GetReducedUserAgent(mobile=true) does NOT emit the Android/
  "Mobile Safari" tokens on a non-IS_ANDROID build, so the mobile string is
  built explicitly.
- Per-navigation desktop override for desktop-gated hosts. WhatsApp Web refuses
  a mobile UA ("open WhatsApp on your phone"). The UA can't be a process-global
  launch decision because the app opens home.html first and the user navigates
  in-session. Fix: Shell::DidStartNavigation (shell.cc) calls
  NavigationHandle::SetIsOverridingUserAgent(host is *.whatsapp.com) +
  WebContents::SetUserAgentOverride(desktop). Works for omnibox/link/redirect.
  navigator.platform shim now derives from the effective UA (Android => armv8l).
  berry-desktop.enable still forces desktop globally.
- Mobile viewport/layout WebPreferences (OverrideWebkitPrefs): viewport_meta,
  shrink-to-fit, mobile viewport style => narrow layout, less paint.
- GPU rasterization: launcher adds --enable-gpu-rasterization in GPU mode so
  tiles raster on the otherwise-idle Adreno 330 instead of stealing Krait cores
  from the single-threaded bootstrap JS.
- --disable-renderer-accessibility (always): no screen reader on BB10, so skip
  building/maintaining the a11y tree on every DOM mutation. Pure CPU back.
- ServiceWorker re-enable behind berry-sw.enable. WhatsApp/PWAs use a SW as
  their primary (Cache Storage) cache. It was lumped into the unstable IPC
  disable-list; re-enabled it tests stability + warm-load caching.
  berry-lowend.enable adds --enable-low-end-device-mode (phone memory/GC/tile
  heuristics) for A/B.

On-device results so far:
- Per-host UA verified: example.com => mobile, web.whatsapp.com => desktop, via
  in-session navigation. WhatsApp shows the real UI (no "use a computer" gate).
- ServiceWorker: stable across 2 tapped launches, 0 SIGSEGV/signals. The old
  20-60s instability attributed to the disable-list did not reproduce for SW.
- WhatsApp worst main-thread stall 12.6s (desktop bundle, GPU raster + a11y off,
  warm caches) vs the 15.3s section-7 baseline (~18% better), but not yet
  isolated per-lever. SW warm-load delta still unquantified (a warm run was
  contaminated by Wi-Fi/DNS flakiness: a 180s idle "stall" + ERR_NAME_NOT_RESOLVED
  / ERR_ADDRESS_UNREACHABLE, not compute).
- The mobile-bundle win applies to GENERAL browsing, not WhatsApp (which stays
  desktop by necessity); WhatsApp's gain is only GPU-raster freeing cores.

## 9. Low-end device mode is a real WhatsApp win (--enable-low-end-device-mode)

A/B on tapped WhatsApp loads (desktop UA, GPU raster, a11y off, SW on, warm),
toggling only berry-lowend.enable:

  rank   low-end OFF   low-end ON
  worst    12.6 s        7.8 s
  2nd       9.5 s        3.9 s
  3rd       9.4 s        3.4 s
  4th       9.2 s        2.6 s
  5th       7.4 s        2.6 s

Worst stall -38%, and the WHOLE distribution collapses (not just the peak), so
this is a real effect, not the 8-51s run-to-run variance. No crash, no net
errors. Interpretation: the desktop WhatsApp bundle creates memory pressure on
the 3 GB device; low-end mode's smaller V8 heap + leaner image/tile caches cut
GC/eviction churn and hand CPU back to the single-threaded bootstrap JS. This is
the largest single WhatsApp load improvement measured so far. Recommend keeping
it on (berry-lowend.enable, or bake as default).

Note this partially revises section 7: the stall is still single-threaded JS,
but it was NOT purely at the silicon ceiling -- memory-pressure side effects were
inflating it, and trimming them (low-end mode) recovers meaningful time.

## 10. "Bad internet" on WhatsApp is flaky QNX DNS, not a stall

WhatsApp intermittently flashed "bad internet" before loading even on strong
Wi-Fi. The ChunkDone probe (logs failures only) showed the main page connected
in ~42 ms (dns=42 connect=88 ssl=58 ttfb=91) -- healthy -- while a few SECONDARY
hosts failed: crashlogs.whatsapp.net (-105 NAME_NOT_RESOLVED), webtp.whatsapp.net
(-2), web.whatsapp.com/status.json (-109 ADDRESS_UNREACHABLE), analytics /ajax/bz
(-2). The failures cluster on *.whatsapp.net (where WhatsApp's realtime/health
checks live), so its watchdog flashes "bad internet" then recovers on retry.
Root cause: the QNX system resolver (getaddrinfo) intermittently drops names even
on a good link, and the device ships NO /etc/resolv.conf (so Chromium's built-in
resolver had no nameservers and fell back to getaddrinfo).

FIX (implemented, services/network/network_service.cc, IS_QNX): right after the
HostResolverManager is created, enable the built-in insecure DNS client and set
DnsConfigOverrides = CreateOverridingEverythingWithDefaults() with explicit
public nameservers (1.1.1.1, 8.8.8.8, 1.0.0.1, 8.8.4.4), secure_dns_mode=off.
This does plain DNS over UDP straight to public resolvers (no DoH bootstrap
needed since they're IPs), bypassing QNX getaddrinfo. enable_built_in_dns is
already on (=use_blink), so no gn change. System resolver remains as fallback.
Verified on-device: "built-in DNS client ON" marker, dns=53ms real resolution,
zero -105/-109 over the run, WhatsApp committed error=0.

## 11. Guaranteed exit on app close (no orphaned content_shell)

Symptom: closing the app (Navigator swipe-up) sometimes left content_shell
running in the background, draining CPU/RAM and slowing the next launch.

Root cause: NAVIGATOR_EXIT (qnx_screen_event_source.cc) -> QnxExitCallback ->
Shell::Shutdown(), which does a GRACEFUL teardown (Close all windows, then
RunLoop().RunUntilIdle()). In --single-process the renderer shares the process,
so a busy page (heavy JS, stuck present loop, spinning challenge) can keep the
loop from going idle and the process never exits.

FIX (shell_platform_delegate_qnx.cc QnxExitCallback + base/qnx_hard_watchdog.*):
on NAVIGATOR_EXIT, arm StartQnxExitWatchdog(3000) -- a detached thread that
_exit(0)s after 3s regardless of message-loop state (dumps all thread stacks on
the deadline for diagnosis) -- then call Shell::Shutdown() and _exit(0)
immediately after it returns instead of waiting on remaining loop iterations.
Net: the app window closing ALWAYS tears down content_shell within ~3s worst
case. This is a stability + perf win (no background CPU drain between sessions).

## 12. Background throttle when thumbnailed/covered (NAVIGATOR_WINDOW_STATE)

The QNX event loop handled only orientation + exit; it ignored
NAVIGATOR_WINDOW_STATE. So when the app was swiped to the multitask card
(THUMBNAIL) or covered by another app (INVISIBLE), Chromium still treated the
page as VISIBLE and kept running requestAnimationFrame, timers, and compositing
at full rate -- burning Krait cores in the background and competing with the
foreground app.

FIX: qnx_screen_event_source.cc now handles NAVIGATOR_WINDOW_STATE and forwards
a visible bit (FULLSCREEN => visible, THUMBNAIL/INVISIBLE => hidden) via a new
QnxScreenVisibilityCallback (qnx_screen_input_callback.*). QnxVisibilityCallback
(shell_platform_delegate_qnx.cc) calls
WebContents::UpdateWebContentsVisibility(VISIBLE/HIDDEN). HIDDEN throttles rAF,
engages background-timer throttling, and stops paint/composite, handing CPU and
battery back to the foreground app; returning to fullscreen restores VISIBLE.
Smoke-verified: new binary launches/navigates/exits cleanly on-device.
