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

## 13. Network-level ad/tracker blocking (the cheapest JS win)

The biggest practical lever for GENERAL heavy sites (news/forums/blogs) is not
making JS faster but running LESS of it: most third-party weight is ads /
analytics / tag managers. Cancelling those requests before they load means the
Krait never downloads, parses, or executes that code.

Implementation (content/shell/common/berry_adblock.{h,cc}): a stateless
blink::URLLoaderThrottle that cancels (ERR_BLOCKED_BY_CLIENT) any request whose
host suffix-matches a curated EasyList/EasyPrivacy-derived core (~60 domains).
Wired at BOTH interception points so all request types are covered:
- renderer ShellContentRendererUrlLoaderThrottleProvider::CreateThrottles ->
  subresources (scripts, XHR/fetch, images, beacons, incl. worker-initiated).
- browser ShellContentBrowserClient::CreateURLLoaderThrottles -> ad IFRAMES
  (which load via the navigation path).
Suffix match is label-boundary aware, so "doubleclick.net" also blocks
"securepubads.g.doubleclick.net". The list DELIBERATELY excludes functional /
auth hosts (accounts/apis.google.com, *.gstatic.com, *.googleapis.com, social
logins, CDNs) so Google sign-in and normal page function are unaffected.

Default ON; disable on-device without a rebuild via BERRY_ADBLOCK=0 or a
/accounts/1000/shared/misc/berry-adblock.disable marker. Set BERRY_ADBLOCK_LOG=1
to log each block to stderr.

Verified on-device (deploy/diag_adblock.html, BERRY_ADBLOCK_LOG=1): 4/4 ad hosts
blocked (google-analytics, googletagmanager w/ query, securepubads.g.doubleclick
subdomain, c.amazon-adsystem), 3/3 functional hosts passed (apis.google.com,
gstatic, example.com). No crash.

Follow-ups (not done): load full EasyList/EasyPrivacy filter lists (cosmetic +
path rules, not just hosts) from a bundled file; a per-site allowlist; counting
blocked bytes for a "data saved" readout.

## 14. PartitionAlloc-as-malloc: builds + links, but hangs at startup (PARKED)

Goal: route malloc/new through PartitionAlloc (faster, lower-fragmentation than
the QNX system malloc -- a direct win for allocation-heavy JS/DOM on the Krait).

State of the world: use_partition_alloc=true already (PA lib compiles & runs on
QNX for its internal partitions). PA-as-malloc additionally needs the allocator
shim, which a gn assert blocked on QNX and which had no QNX code.

Build enablement (DONE, links cleanly):
- partition_alloc.gni: add is_qnx to the use_allocator_shim assert allowlist.
- shim/allocator_shim.cc: no change -- QNX already routes to cpp_symbols +
  (via #else) libc_symbols; glibc_weak_symbols is gated behind LIBC_GLIBC, and
  allocator_shim_internals.h falls back to empty/noexcept __THROW off glibc.
- shim BUILD.gn: add an is_qnx branch listing cpp_symbols + libc_symbols.
- allocator_shim_override_libc_symbols.h: the ONLY source conflict was cfree --
  QNX declares `int cfree(void*)`, not void, so guard an int-returning override
  for IS_QNX (functions can't differ only by return type).
- build_overrides/partition_alloc.gni: `|| is_qnx` flips both defaults on.
With those, gn reports use_allocator_shim=true, use_partition_alloc_as_malloc=
true, BRP off, and content_shell builds and LINKS with no undefined symbols.

Runtime BLOCKER (why it's parked): the linked binary wedges in very early PA
init. content_shell launches with a SINGLE thread (tid 1) pegged at ~100% CPU
(utime climbs ~1s/s) and emits ZERO output -- it never reaches thread-pool /
logging / SIGUSR2-sampler setup, so no on-device backtrace is obtainable (no
gdbserver deployed). SpinningMutex uses a pthread mutex on QNX (POSIX fast
mutex), so the spin is NOT the lock; most likely the 32-bit PartitionAddressSpace
/ pool reservation or an early CAS/retry loop that needs a genuine PA-internals
port for 32-bit QNX (has_64_bit_pointers=false). Reverted to system malloc so the
browser keeps working; the inert enablement (assert allowlist, cfree fix, shim
BUILD.gn branch) is kept. To retry: re-add `|| is_qnx` in build_overrides, deploy
gdbserver + cross-gdb, and backtrace the spinning thread.

## 15. Self-contained .bar + user-facing settings via launcher markers

The browser now ships as one self-contained `.bar`
(`deploy/berry-shell-bar/BerryBrowser.bar`, descriptor `bar-descriptor.xml`)
built with the BB10 NDK packager:
```
source /root/bbndk/bbndk-env_10_3_1_995.sh
cd deploy/berry-shell-bar
bash ../build-launcher.sh                 # rebuild launcher (QNX ARM)
# refresh payload/ from out/qnx-arm (content_shell + paks + snapshot + icu)
blackberry-nativepackager -package BerryBrowser.bar bar-descriptor.xml
```

Two correctness fixes made the .bar work on a CLEAN install (no SSH hot-swap):
- The descriptor now stages the engine as `content_shell.exe` (the launcher
  execs `<dir>/content_shell.exe`); previously it shipped as `content_shell`,
  so a fresh install only ran after `deploy-binary.sh` pushed `.exe` over SSH.
- `payload/` is refreshed from the current `out/qnx-arm` build so the bundled
  binary carries the ad-block + guaranteed-exit + background-throttle work
  (the staged payload had been stale from the mic build).

Settings are exposed as **launcher marker files** in
`/accounts/1000/shared/misc/` (read each app start, mapped to built-in Chromium
switches, no rebuild needed). New in launcher.c:
- `berry-noimages.enable` -> `--blink-settings=imagesEnabled=false`
- `berry-nojs.enable`     -> `--blink-settings=scriptEnabled=false`
- `berry-dark.enable`     -> `--blink-settings=forceDarkModeEnabled=true`
- `berry-ua` (text)       -> `--user-agent=<verbatim>` (suppresses mobile-UA)
Multiple content/appearance toggles coalesce into one `--blink-settings`.
Full reference: `deploy/berry-shell-bar/BROWSER-SETTINGS.md`.

## 16. BerryBrowserV3: in-app restart, resolution presets, custom landing page

Shipped as a SEPARATE package (`bar-descriptor-v3.xml`,
id `com.sw7ft.BerryShellV3`, name `BerryBrowserV3`, output `BerryBrowserV3.bar`)
so it installs alongside the existing app instead of replacing it.

In-app "Restart browser" (so a resolution change can be applied without manually
closing/reopening). Design choice: keep the PROVEN pid model. The launcher still
`execv`s content_shell (content_shell stays the Navigator-launched pid that owns
the qnx_screen window group `berryshell_<pid>`), and RESTART is done engine-side:
- shell.cc `DidStartNavigation`: a navigation to host `berry.restart` posts
  `BerryRestartRelaunch()` (deferred off the observer callback).
- `BerryRestartRelaunch()`: arms the exit watchdog, `Shell::Shutdown()` (releases
  the window/group cleanly), then `execv("<dir>/launcher")` — same pid, so the
  relaunch re-reads every marker (resolution/landing/UA/flags) with the proven
  window model. Falls back to `_exit(42)` if exec fails.
- The Restart control lives in the .bar's `home.html` (a tile -> berry.restart),
  matching the "URL/controls shipped in the .bar" intent. URL bar `berry.restart`
  works too. (A launcher fork-supervisor was rejected: it would make even the
  FIRST launch's window come from a child pid — too risky for the primary path.)

Why the URL bar stays in content_shell: it's a Skia toolbar composited with the
page (`berry_browser_chrome.cc`); the launcher has no window. A native overlay
toolbar would be a large QNX windowing rewrite for no real gain.

Resolution presets (launcher markers, apply on Restart): `berry-x-420` /
`berry-x-720` (default) / `berry-x-1440` (native panel, no downscale).

Custom landing page (launcher, no rebuild): `berry-home-url` (text start URL,
bare host gets https://) > `berry-home.html` in shared/misc (editable) > bundled
home.html.

## 17. In-app Settings page (engine-served) + 1440 default

Replaces the "edit marker files over SSH" flow with a real on-device Settings UI,
since web pages are sandboxed and can't write shared/misc but content_shell can.
Same sentinel pattern as restart, all in shell.cc `DidStartNavigation` (QNX):
- `berry.settings` -> engine builds an HTML page from the CURRENT marker state
  (BerryBuildSettingsHtml), writes it to `shared/misc/.berry-settings.html`, and
  LoadURLs it. Toggles/switches are links/forms to `berry.set`.
- `berry.set?k=KEY&v=VAL` -> BerryHandleSet writes/removes the marker
  (BerrySetMarker / BerrySetTextMarker for ua/home), then re-renders the page so
  state is always accurate. Resolution radio clears berry-x-* then sets the chosen
  one. Adblock toggle maps to presence of berry-adblock.disable (inverted).
- `berry.home` -> loads the bundled home.html (derives app dir from exefile).
Settings are startup flags, so the page has an "Apply & Restart" button
(-> berry.restart). Landing page (home.html) redesigned with a gear (Settings)
and restart button + Settings/Restart action tiles.

Default resolution is now 1440² (launcher render default; native panel, no
downscale). Override in Settings > Display (420/720/1440).
