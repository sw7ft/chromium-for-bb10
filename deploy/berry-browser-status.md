# BerryBrowserV3 — Project Status (Sep 6 2026)

**Device:** BlackBerry Passport (QNX / BB10) — other BB10 models auto-detected since build 84 (see *Device compatibility*)  
**Engine:** Chromium `content_shell` (~96 MB ARM), single-process default  
**App:** BerryBrowserV3 `.bar` (`com.sw7ft.BerryShellV3`)  
**Latest build:** **84** — panel auto-detect (touch calibration on Q10/Q20/etc.), modern start page, Android-first YouTube playback  
**Log:** `/accounts/1000/shared/misc/berry-kbd.log`

This document is the consolidated “where we are” snapshot and **onboarding guide for the next agent**. Read **Executive summary** + **Core strategy**, then **Agent continuation guide**, **Diagnostics**, and **Build pipeline** before touching code.

Detailed investigations live in linked handoff files at the bottom.

---

## Executive summary

BerryBrowserV3 is a real Chromium browser on a 2014 phone. The winning pattern is **not** “make hostile sites cooperate” — it is **route around** them with **body-swap shims** (innertube JSON + own HTML/JS) while keeping Google as default search with a **soft landing** when the robot wall fires.

| Category | Status |
|----------|--------|
| YouTube **watch** (home tile) | ✅ Working (watch shim) |
| YouTube **search** (search tile) | ✅ Working (search shim, build 47+) |
| Google search | 🟡 **Intermittent** — often works; `/sorry` 429 when reputation gate fires |
| Google `/sorry` robot wall | ✅ **Build 49** — soft-bounce replaces spinner (DDG + retry Google) |
| reCAPTCHA checkbox | ❌ **Tabled** — unwinnable once on `/sorry` (poisoned `s=` token) |
| YouTube **home/browse** (`youtube.com/`) | ❌ By design — full kevlar/SABR; do not tune |
| WhatsApp Web / heavy SPAs | 🟡 Loads with disk cache; long JS/GC stalls |
| Embedded Google Maps (3rd-party sites) | 🟡 **Build 52** — session desktop UA fix; test hihostels.ca |
| Google Maps (home tile) | 🟡 Geolocation override exists; verify tiles after build 52 |
| X.com / Facebook | ❌ Blob URL crashes (SIGSEGV) |
| Mic / `record_audio` permission | 🟡 Code ready; may need `.bar` reinstall for manifest |
| Automated SSH testing | 🟡 Headless bundle in `berry-sshtest` works; no node on device |

---

## Core strategy (read this first)

### Mechanism vs adversary

| Type | Example | Fix |
|------|---------|-----|
| **Mechanism** | Watch/search shims, sorry soft-bounce, decode bugs | Log pipeline → patch → body-swap |
| **Adversary** | YouTube kevlar browse, Google BotGuard/reCAPTCHA cure | Route around; do not fight |

### What works on YouTube

We do **not** run YouTube’s desktop kevlar app. We intercept specific URLs and render from **innertube JSON**:

1. **Watch shim** — `/watch?v=` → fetch player JSON → strip SABR → play progressive MP4  
2. **Search shim** — `/results?search_query=` → innertube search API → tile list  

Both spoof `visitorData` and Android VR client context where needed.

### What we deliberately do not fix

- **`www.youtube.com/` home feed** — kevlar + SABR + botguard; same class as m.youtube.com  
- **Passing Google reCAPTCHA checkbox** — server sends poisoned state; transport is clean  
- **TLS/JA3 spoofing** — structural hypothesis largely excluded; not worth the cost  

---

## Device compatibility (build 84+)

The engine is device-agnostic; what varies per model is **panel size, touch
mapping, and render resolution**. Since build 84 the launcher queries libscreen
for the native panel size at startup and derives all three automatically —
before that, everything defaulted to Passport geometry and touch on 720-panel
devices landed at half position (the Q10/Q20 "calibration" bug).

| Device | Panel | Default render | Notes |
|--------|-------|----------------|-------|
| Passport | 1440×1440 | 720² (540² tier default) | Primary target; fully tested |
| Classic (Q20) | 720×720 | 540² | Auto-detected; `q20` and `classic` both accepted |
| Q10 / Q5 | 720×720 | 540² | Auto-detected |
| Z10 | 768×1280 | 384×640 | Auto-detected; portrait, less tested |
| Z30 / Z3 / Leap | 720×1280 | 360×640 | Auto-detected; portrait, less tested |
| Unknown panel | detected W×H | half of panel (min 320) | Fallback: uses whatever libscreen reports |

How it resolves, in order:
1. **`berry-device` marker** (written by Settings > Device) — manual pick always wins.
2. **libscreen auto-detect** — no marker or marker says `auto`: read the first
   display's `SCREEN_PROPERTY_SIZE`, match a known profile (either orientation),
   else synthesize output=panel, render=panel/2.
3. **Passport fallback** — only if detection itself fails.

The launcher exports `QNX_SCREEN_OUTPUT_*` (panel) vs `QNX_SCREEN_WIDTH/HEIGHT`
(render); `QnxScreenMapOutputToRender()` scales incoming touch coordinates
between them, so a wrong output size directly mis-places every tap.
`berry-device-rotation` marker still overrides rotation (0/90/180/270).
Settings > Device shows **Auto (detect)** as the default; the launcher logs
`device = <name> (auto) panel=WxH` at startup for verification.

Caveats: only the Passport is regression-tested on real hardware. Keyboard
models (Q10/Q20) use the same BPS input path so hardware keys should work
unchanged. Z-series portrait devices share the same rotation default (90) as
the square phones — if a Z10 renders sideways, set rotation via marker. Minimum
OS is BB10 10.3.x (QNX 8 userland, `libscreen.so.1`).

---

## Current build (72) — on device

| Item | Value |
|------|-------|
| Launcher string | `BerryShell: BerryBrowserV3 build 72` |
| Binary path | `.../BerryShellV3.testRel_erryShellV3d55e24f1/app/native/content_shell.exe` |
| Cookies DB | `.../data/content_shell_data/Cookies` (~28 KB, NID/AEC/SNID persist) |
| `.bar` staged | `/accounts/1000/shared/misc/BerryBrowserV3-3.0.2-build49.bar` |

**Build 49 adds:** Google `/sorry` **soft-bounce shim** — intercepts main-frame `google.com/sorry/*`, body-swaps a local page:

- **Search DuckDuckGo** — query parsed from `continue=` URL param  
- **Try Google again** — re-issues same `q=` (transient blocks often clear on retry)  
- Disable: `touch .../berry-google-sorry-shim.disable`  
- Log grep: `SorryBounce` in `berry-kbd.log`

---

## Build changelog (recent)

| Build | What shipped |
|-------|----------------|
| **37–45** | Watch shim — innertube player, SABR strip, allowlist, tripwire |
| **46** | Search placeholder tile, nav tripwire on watch leave |
| **47** | **YT Search shim** — `/results?search_query=`, search API spoof |
| **48** | Generic **`berry-decode.debug`** probe (`DecodeProbe head/body`) |
| **49** | **Google `/sorry` soft-bounce** — P1 prevention UX |
| **50** | **Perf:** TLS BAD_RECORD_MAC auto-retry; blob content-type default; safer BerryNav URL logging |
| **51–52** | **Maps:** per-host UA (not session-wide desktop); clear UA when leaving YouTube/Maps; WebGL errors → console via `berry-maps.debug`; Maps tile URL logging; `diag_maps.html` probe |
| **59** | Baseline maturation build (mobile UA, low-end mode, SW, adblock, GPU raster defaults baked in) |
| **60** | **Perf defaults:** telemetry block + privacy-sandbox cuts ON by default; HTTP/media cache 64→96MB |
| **61** | **Messenger Web** home tile; desktop UA for messenger.com + Facebook login/oauth paths |
| **62** | **Perf bundle:** 540² Passport default; 256MB disk/media cache; Meta telemetry blocklist; Settings **Perf** preset (540+12fps+2thr); home page preconnect hints |
| **63** | **QUIC default ON**; **WASM liftoff-only** default (no TurboFan tier-up); passive **QNX:BF peak** log every 60s |
| **64** | **Fix Messenger login:** stop blocking `graph.facebook.com` (Graph API required for auth); keep analytics-only blocklist |
| **65** | **Fix Messenger login:** WASM liftoff-only reverted to opt-in (default V8 tier-up for login crypto) |
| **66** | **Fix Messenger verify flow:** desktop UA on all `www.facebook.com` (checkpoint/verify redirects were mobile → bogus "wrong password") |
| **72** | **X/Twitter login (WebGPU):** hide `navigator.gpu` (was present but non-functional — "Failed to create WebGPU Context Provider"). X's `ondemand.castle.js` feature-detected WebGPU, tried it, threw 2× `Error`, aborting its anti-bot token → login rejected with "Something went wrong". Reporting WebGPU honestly absent lets castle take the no-WebGPU path. |
| **71** | **X/Twitter login fingerprint hygiene:** implemented QNX core-count in WebRTC `cpu_info.cc` (was `No function to get number of cores` → returned 1); fingerprint shim now clamps `navigator.hardwareConcurrency`≥4 and `deviceMemory`=4 (low-end mode reported outlier values that X's `ondemand.castle.js` anti-bot module weighs). NOTE: adversary-class like FB/YT gates — reduces the "weird browser" score but may still require email/2FA device verification. X page no longer SIGSEGVs (build 50 blob fix holds). |
| **70** | **YouTube home bounce:** `youtube.com/` (main frame) body-swaps a local dark landing page — search box → search shim → watch shim. Kevlar home app no longer half-loads with an error. Escape hatch: `youtube.com/?berry_full=1`. Log: `YtHomeBounce`. |
| **69** | **YouTube embeds on third-party sites:** watch shim now also intercepts `/embed/<id>` (Bing Videos & co. iframes — real YT player showed "An error occurred"), `/shorts/<id>`, `/v/<id>`, and `youtube-nocookie.com`; embed/shorts/v shimmed in main frames AND subframes; scrub `X-Frame-Options` so shim HTML renders inside iframes. |
| **68** | **Fix video media timeouts (CBC, googlevideo):** QUIC back to OPT-IN. QUIC TLS proof verification fails on QNX (BoringSSL `CERTIFICATE_VERIFY_FAILED` spam from quiche `tls_handshaker.cc`; ignore-cert-errors wrapper covers TCP but not the QUIC handshake outcome), so HTTP/3-advertising CDNs media-timeout. Was masked Jul 10–Aug 31 by the stale `berry-quic.disable` marker; broke immediately once QUIC actually ran. Settings toggle now writes `berry-quic.enable`. |
| **67** | **Fix silent flag loss:** merged `--disable-features` into ONE switch (Chromium keeps only the last occurrence — privacy-sandbox cuts + Alt-Svc marker were being dropped since build 60). **Fix Messenger OOM crash:** default `--max-old-space-size=256` (low-end mode's ~128MB JS heap cap OOM-killed Messenger, seen Aug 27). **Stall log rework:** viz-side `QNX:BF peak` (false multi-hour peaks from idle/background gaps) replaced by 500ms renderer main-thread heartbeat `QNX:MT stall` (ignores gaps ≥60s). Also removed stale `berry-quic.disable` + `berry-block.disable` markers left on device since Jul 10 debugging (QUIC + telemetry blocklist active again). |

Older builds (27–29) remain in `shared/misc` for sideload; current app is V3 package.

---

## What works (verified)

### YouTube watch (home → YouTube tile)

```
WatchShim js-alive → InnertubeSpoof → PlayerFormats itag18 → decBody=629172 (playback)
```

Automated over SSH (headless, no human tap):

```bash
ssh passport 'cd /accounts/1000/shared/misc/berry-sshtest && \
  LD_LIBRARY_PATH=./lib ./content_shell --ozone-platform=headless --headless \
  "https://www.youtube.com/watch?v=jNQXAC9IVRw"'
```

### YouTube search (search tile → results shim)

Innertube `/youtubei/v1/search` spoof; `videoRenderer` tiles only.

### General browsing

- Wikipedia, many static/light sites  
- DuckDuckGo HTML (`html.duckduckgo.com/html`) — default omnibar fallback  
- Disk HTTP cache + cookie persistence (QNX profile)  
- In-app Settings (`berry.settings`) — markers without rebuild  

### Google search (intermittent)

User report: **“works most of the time.”** Evidence:

- Same session: `q=hello` → results; `q=weather` → `/sorry` (reputation gate, not structural)  
- Client Hints fix once moved verdict: search returned **HTTP 200** (Pass 3)  
- Cookies persist (NID, AEC, SNID on device)  

When `/sorry` fires, **build 49** should show bounce page instead of reCAPTCHA spinner.

---

## Known troubles

### 1. Google `/sorry` + reCAPTCHA (prevention vs cure)

**Prevention (open / partially done):**

- Intermittent reputation/velocity gate — debug bursts (`q=hi` loops) make it look worse  
- Cookie persistence **shipped** — reduces frequency, not zero  
- **Soft-bounce shipped build 49** — makes wall harmless when it fires  
- **Not done:** consent cold-start (SOCS/CONSENT on fresh profile); passive hit-rate logging  

**Cure (tabled — do not invest):**

- Checkbox spinner → `SyntaxError` on poisoned `s=` token → `reCAPTCHA Timeout`  
- All assets HTTP 200, bodies decode clean; zero `reload`/`userverify` XHR  
- reCAPTCHA is **Google hosted JS**, not Chromium — cannot “build it in”  

See: `/root/berry-agent-handoff-recaptcha-for-next-agent.md`

### 2. YouTube home / browse (`youtube.com/`)

Full kevlar desktop app — dozens of JS bundles, SABR preview URLs, CORS garbage. Slow skeleton, broken thumbnails. **Not a shim regression.** Use watch + search tiles instead.

### 3. Social sites — blob URL crashes

X.com and Facebook historically hit `blob:` URLs → **SIGSEGV** (often `std::string::append` on QNX libstdc++). Build **50** hardens blob response headers (default `application/octet-stream`) and fixes dangling `substr().c_str()` in BerryNav logs. Still monitor with `grep CRASH berry-kbd.log` after X/Facebook sessions.

### 4. WhatsApp Web / heavy SPAs

- Disk cache helps (~81% warm reload)  
- Headline stall is **JS/GC-bound**, not WASM compile  
- Multi-second `QNX:BF` main-thread freezes on Passport hardware  

### 5. Compiler / libcow strings (historical)

clang 17 + GCC 9.3 libstdc++ COW strings → rare SIGSEGV in `std::operator+`. `-D_GLIBCXX_USE_CXX11_ABI=1` mitigates. Watch for regressions on heavy string sites.

### 6. Mic / Gmail / permissions

- `record_audio` added to manifest in descriptor; installed app may predate it  
- Mic pipeline + QSA fallbacks exist in tree  
- Full `.bar` reinstall needed for permission manifest update  

### 7. Deploy / automation friction

| Method | Status |
|--------|--------|
| `deploy-binary.sh` + root scp to app native dir | ✅ Fast engine updates (~30s) |
| scp `.bar` → `shared/misc` → tap Install | ✅ Works; manual tap |
| `blackberry-deploy` | Needs device dev password; USB IP `169.254.0.1` reachable |
| SSH exec installed app binary | ❌ Permission denied (sandbox) |
| Headless `berry-sshtest` bundle | ✅ Automated URL tests |
| `berry-daemon` / CDP remote | Needs node on device (not present) |

### 8. Agent-relay doc drift

Cookie persistence was shipped but omitted from early CAPTCHA handoffs — nearly re-litigated as “untried.” **Always verify code + device**, not only markdown.

---

## Architecture (one page)

```
┌─────────────────────────────────────────────────────────┐
│  BerryBrowserV3 .bar                                     │
│  launcher.c → flags, markers, UA, sp/GPU defaults        │
│       ↓ exec content_shell.exe                           │
│  Chromium content_shell (single-process on QNX)          │
│       ↓                                                  │
│  network/url_loader.cc (QNX shims)                       │
│    • /watch?v=        → WatchShim HTML + innertube JS    │
│    • /results?search_query= → SearchShim                 │
│    • google.com/sorry → SorryBounce (build 49)           │
│    • /youtubei/v1/player → visitorData spoof, SABR strip│
└─────────────────────────────────────────────────────────┘

Markers: /accounts/1000/shared/misc/berry-*
Logs:    /accounts/1000/shared/misc/berry-kbd.log
```

---

## User-facing routes (recommended)

| Want | Do |
|------|-----|
| Watch a video | Home → **YouTube** tile (watch shim) |
| Search YouTube | Home → **YT Search** tile |
| Web search | Omnibar → DuckDuckGo (default) or Google (intermittent) |
| Google blocked | Build 49 bounce → **DuckDuckGo** or **Try Google again** |
| Settings | `berry.settings` or gear on home page |
| Debug transport | `touch .../berry-decode.debug` → grep `DecodeProbe` |

---

## Diagnostics — how we get signal

Everything flows to **`/accounts/1000/shared/misc/berry-kbd.log`** when the user launches from the home tile. The `.bar` launcher (`launcher.c`) redirects `content_shell` stderr there (append mode, shared group-readable).

### Log sources (what shows up in `berry-kbd.log`)

| Prefix / pattern | Source | When enabled |
|------------------|--------|--------------|
| `BerryShell:` | `launcher.c` | Every app start (build #, UA, flags, landing page) |
| `BerryNav:` | `QNX_NAV_LOG*` in network/nav code | Marker or env (see below) |
| `WatchShim` / `SearchShim` / `SorryBounce` | `url_loader.cc` shims | Always when shim fires |
| `InnertubeSpoof`, `PlayerFormats`, `decBody=` | YouTube pipeline | Nav debug on |
| `DecodeProbe` | `url_loader.cc` + `navigation_url_loader_impl.cc` | `berry-decode.debug` marker |
| `QNX:CRASH`, `SIGSEGV` | crash hooks | Always on crash |
| `QNX:BF`, `QNX:FPS` | main-thread / frame timing | `berry-fps.enable` |
| `CONSOLE(...)` | Blink JS console | Nav debug on (verbose) |

Implementation: `base/qnx_trace.h` — markers checked once at startup via `access()`.

### Debug markers (touch on device, relaunch app)

```bash
ssh passport 'touch /accounts/1000/shared/misc/berry-nav.debug'    # BerryNav + CONSOLE (heavy)
ssh passport 'touch /accounts/1000/shared/misc/berry-kbd.debug'    # nav + GL verbose
ssh passport 'touch /accounts/1000/shared/misc/berry-decode.debug' # DecodeProbe head/body hex
ssh passport 'touch /accounts/1000/shared/misc/berry-fps.enable'   # frame/pump timing
ssh passport 'touch /accounts/1000/shared/misc/berry-maps.debug'   # WebGL errors → CONSOLE
ssh passport 'rm -f /accounts/1000/shared/misc/berry-*.debug'    # quiet again
```

SSH env overrides (headless bundle only): `QNX_NAV_DEBUG=1`, `QNX_GL_DEBUG=1`, `QNX_FPS=1`.

### Pull logs from host

```bash
# Quick tail
ssh passport 'tail -200 /accounts/1000/shared/misc/berry-kbd.log'

# Full log to host
ssh passport 'cat /accounts/1000/shared/misc/berry-kbd.log' > /root/berry-kbd-full.log

# Filtered CAPTCHA/Google session (script in tree)
/root/chromium/src/deploy/pull-captcha-log.sh --live 120 /root/berry-session.log
/root/chromium/src/deploy/pull-captcha-log.sh --since-mark /root/berry-session.log

# Shim smoke (after user taps YouTube tile ~30s)
/root/chromium/src/deploy/smoke-youtube-shim.sh passport 800000

# Google load test (headless bundle, automated)
/root/chromium/src/deploy/test-google.sh --headless --search --no-deploy
```

### Useful grep patterns

```bash
# YouTube watch shim PASS signature
grep -E 'WatchShim|js-alive|InnertubeSpoof|PlayerFormats|decBody=[1-9]' berry-kbd.log

# Search shim
grep -E 'SearchShim|search_query' berry-kbd.log

# Google sorry / bounce
grep -E 'SorryBounce|sorry|429|reCAPTCHA|SyntaxError' berry-kbd.log

# Maps (build 52+: expect khms or maps/vt after UA = mobile)
grep -E 'BerryNav: UA =|khms|maps/vt|MAPSDIAG|WebGL|Maps geolocation' berry-kbd.log

# Transport clean (decode probe)
grep DecodeProbe berry-kbd.log

# Crashes
grep -E 'CRASH|SIGSEGV|fault=' berry-kbd.log

# Confirm build on device
grep 'BerryShell: BerryBrowserV3 build' berry-kbd.log | tail -1
```

### On-device HTML probes (push to `shared/misc`, open via `file://`)

| File | Tests |
|------|-------|
| `deploy/diag_maps.html` | WebGL1/2 probe + minimal Maps embed (uses hihostels public API key) |
| `deploy/decompress_probe.html` | Compression Streams + WebCrypto |
| `deploy/fp_probe.html` | `navigator.webdriver`, `window.chrome`, plugins |
| `deploy/wasm_probe.html` | WASM + crypto in page |
| `deploy/worker_probe.html` | Dedicated worker + fetch |

```bash
scp chromium/src/deploy/decompress_probe.html \
  passport:/accounts/1000/shared/misc/
# In browser: file:///accounts/1000/shared/misc/decompress_probe.html
```

### Automated tests without human tap

**Headless bundle** (devuser can exec; installed app binary cannot):

```bash
ssh passport 'cd /accounts/1000/shared/misc/berry-sshtest && \
  LD_LIBRARY_PATH=./lib ./content_shell --ozone-platform=headless --headless \
  "https://www.youtube.com/watch?v=jNQXAC9IVRw" 2>/tmp/headless.log; \
  grep -E "WatchShim|decBody" /tmp/headless.log'
```

Note: headless ≠ tapped GPU path (EGL/WhatsApp differ). Use for **shim/network** signal, not GPU regressions.

### Verify installed build matches host

```bash
# Build string in launcher
ssh passport-root 'dd if=.../app/native/launcher bs=4096 2>/dev/null' | strings | grep "build 49"

# Binary size (should match out/qnx-arm/content_shell)
ls -la /root/chromium/src/out/qnx-arm/content_shell
ssh passport-root 'ls -la .../app/native/content_shell.exe'

# Cookies present (Google persistence)
ssh passport-root 'ls -la .../data/content_shell_data/Cookies'
```

### What NOT to do when diagnosing

- Do **not** `rm Cookies` during Google tests unless testing cold-start explicitly (`test-google.sh` does this — avoid for normal repro).
- Do **not** burst-search `q=hi` in a loop — trains `/sorry` reputation down; invalidates hit-rate measurement.
- Do **not** assume log silence = failure — verbose nav requires `berry-nav.debug`.
- Do **not** trust handoff docs alone — **verify on device** (agent-relay drift burned us on cookies).

---

## Build pipeline — end to end

### Host environment

| Path | Purpose |
|------|---------|
| `/root/chromium/src/` | Chromium tree (QNX patches in-tree) |
| `/root/chromium/src/out/qnx-arm/` | GN output dir (`args.gn` below) |
| `/root/qnx800/` | QNX SDP 8.0 sysroot |
| `/root/bbndk/bbndk-env_10_3_1_995.sh` | BB10 NDK env (Java + `blackberry-nativepackager`) |
| `/root/v8_build/depot_tools/` | `gn`, `ninja` (on PATH) |
| `/usr/lib/llvm-17` | Host Clang for cross-compile |

Full toolchain notes: `/root/toolchain-info.md`

### GN args (`out/qnx-arm/args.gn`)

Key flags: `target_os = "qnx"`, `target_cpu = "arm"`, `is_clang = true`, `use_ozone = true`, `ozone_platform = "qnx_screen"`, `ozone_platform_headless = true`, single static `content_shell` (not component build).

Regenerate if needed:

```bash
cd /root/chromium/src
gn gen out/qnx-arm
```

### Step 1 — Compile `content_shell` (~60–120 min cold, ~1–5 min incremental)

```bash
cd /root/chromium/src
ninja -C out/qnx-arm content/shell:content_shell
# Output: out/qnx-arm/content_shell  (~96 MB ARM ELF)
```

**Shim JS changes** trigger `gen_watch_shim_header` / `gen_search_shim_header` automatically via `services/network/BUILD.gn` → `gen_shim_header.py` (runs `node --check` on JS).

Sorry-bounce HTML is inline C++ in `url_loader.cc` — no JS header step.

**Typical edit → rebuild targets:**

| You changed | Rebuild |
|-------------|---------|
| `watch_shim.js` / `watch_shim_search.js` | `ninja -C out/qnx-arm content/shell:content_shell` |
| `url_loader.cc` (any shim) | same |
| `launcher.c` only | skip ninja; run `build-v3-bar.sh` (compiles launcher with QNX gcc) |
| `bar-descriptor-v3.xml`, `home.html` | `build-v3-bar.sh` only |

### Step 2 — Bump build number (user-visible version)

Edit **both**:

1. `deploy/berry-shell-bar/bar-descriptor-v3.xml` → `<buildId>N</buildId>`
2. `deploy/berry-shell-bar/launcher.c` → `BerryBrowserV3 build N` log line

### Step 3 — Package `.bar`

```bash
cd /root/chromium/src
./deploy/build-v3-bar.sh
# Copies out/qnx-arm/content_shell → berry-shell-bar/payload/
# Compiles launcher with arm-blackberry-qnx8eabi-gcc
# Runs blackberry-nativepackager
# → deploy/berry-shell-bar/BerryBrowserV3-3.0.2-build<N>.bar
```

Optional post-build smoke prompt: `BERRY_SMOKE=1 ./deploy/build-v3-bar.sh`

**BAR rules:** Do not bundle `ldqnx.so.2`. Launcher execs `content_shell.exe` from app native dir. See `/root/chromium-for-bb10/docs/BAR-DEPLOYMENT.md`.

### Step 4 — Deploy to Passport

**Fast path (engine only, ~30s)** — for `content_shell` / shim changes:

```bash
APP_NATIVE="/accounts/1000/appdata/com.sw7ft.BerryShellV3.testRel_erryShellV3d55e24f1/app/native"

# Kill running browser
ssh passport-root 'killall content_shell.exe 2>/dev/null; slay -9 content_shell 2>/dev/null'

# Atomic deploy (needs root — devuser cannot write app native dir)
scp /root/chromium/src/out/qnx-arm/content_shell \
  passport-root:${APP_NATIVE}/content_shell.exe.new
ssh passport-root "cd '${APP_NATIVE}' && mv -f content_shell.exe.new content_shell.exe && chmod +x content_shell.exe"

# If launcher/build string changed:
scp /root/chromium/src/deploy/berry-shell-bar/launcher \
  passport-root:${APP_NATIVE}/launcher.new
ssh passport-root "cd '${APP_NATIVE}' && mv -f launcher.new launcher && chmod +x launcher"
```

Wrapper script (update `PASSPORT_APP_NATIVE` — default path in script is stale):

```bash
PASSPORT_APP_NATIVE="$APP_NATIVE" /root/chromium/src/deploy/deploy-binary.sh
```

**Full `.bar` install** — manifest, permissions, bundled HTML, icons:

```bash
scp deploy/berry-shell-bar/BerryBrowserV3-3.0.2-build49.bar \
  passport-direct:/accounts/1000/shared/misc/
# On device: Files → shared/misc → tap .bar → Install
# Or: blackberry-deploy -installApp -device 169.254.0.1 -password <dev-pw> ...
```

### Step 5 — Verify on device

1. Force-kill app, relaunch from home tile  
2. `grep 'BerryShell: BerryBrowserV3 build' berry-kbd.log`  
3. Run feature-specific grep (see Diagnostics)  
4. For watch shim: `./deploy/smoke-youtube-shim.sh`

---

## Agent continuation guide

**Read first (15 min):**

1. This file — especially **Core strategy** and **Known troubles**  
2. `/root/berry-agent-handoff-recaptcha-for-next-agent.md` — Google gate (prevention vs cure)  
3. `/root/berry-agent-handoff-build45-status.md` — watch OK vs YouTube home broken  

**Before claiming something is broken or untried:**

```bash
grep -r "your hypothesis" /root/chromium/src/   # code
ssh passport-root 'ls -la .../Cookies; strings .../launcher | grep build'  # device
```

### First-session checklist

```bash
# 1. Confirm SSH
ssh passport 'echo ok'
ssh passport-root 'echo ok'

# 2. Confirm installed build
ssh passport-root 'dd if=/accounts/1000/appdata/com.sw7ft.BerryShellV3.testRel_erryShellV3d55e24f1/app/native/launcher bs=4096 2>/dev/null' | strings | grep BerryBrowserV3

# 3. Pull recent log
ssh passport 'tail -50 /accounts/1000/shared/misc/berry-kbd.log'

# 4. Confirm host tree builds (incremental)
cd /root/chromium/src && ninja -C out/qnx-arm content/shell:content_shell
```

### Where to implement common tasks

| Task | Where |
|------|-------|
| New URL body-swap shim | `url_loader.cc` + optional `services/network/qnx/*.js` + `BUILD.gn` gen action |
| Launcher flags / markers | `deploy/berry-shell-bar/launcher.c` + `BROWSER-SETTINGS.md` |
| UA / Client Hints / cookies | `shell_content_browser_client.cc`, `shell_browser_context.cc` |
| Home page tiles / UX | `deploy/berry-shell-bar/home.html`, `youtube-search.html` |
| In-app settings | settings HTML generated into `shared/misc/.berry-settings.html` |
| Nav/logging | `base/qnx_trace.h`, marker files in `shared/misc` |

### Locked decisions (do not re-open without new evidence)

- **No kevlar home tuning** — route to watch/search shims  
- **No reCAPTCHA cure** — soft-bounce (build 49) is the UX fix  
- **No m.youtube.com**  
- **Google gate is intermittent reputation**, not structural (§6b in recaptcha handoff)  
- **Mechanism problems** → log + fix pipeline; **adversary problems** → route around  

### Safe workflows

| Goal | Workflow |
|------|----------|
| Fix YouTube playback | Edit shim JS → ninja → root deploy binary → smoke script |
| Fix Google sorry UX | Edit `BerryBuildSorryBounceHtml` in `url_loader.cc` → ninja → deploy |
| Change default UA/GPU | Marker file OR launcher.c → bar or launcher deploy |
| Test without ~2hr rebuild | Markers + log grep; headless berry-sshtest for shims |
| Ship to user | Bump buildId → build-v3-bar.sh → scp .bar to shared/misc |

### SSH hosts (`~/.ssh/config`)

| Host | User | Address | Use |
|------|------|---------|-----|
| `passport` | devuser | host.docker.internal:2222 | markers (some), headless, scp misc |
| `passport-direct` | devuser | 169.254.0.1 | USB tether, fast scp |
| `passport-root` | root | host.docker.internal:2222 | **appdata deploy**, Cookies, logs (the `root@passport` form FAILS — wrong key; root key is `id_rsa4x`) |

### Key paths on device

```
/accounts/1000/shared/misc/berry-kbd.log          # main log
/accounts/1000/shared/misc/berry-*.enable|debug   # markers
/accounts/1000/shared/misc/berry-sshtest/         # headless test bundle
/accounts/1000/appdata/com.sw7ft.BerryShellV3.testRel_erryShellV3d55e24f1/
  app/native/content_shell.exe                    # runtime binary
  app/native/launcher                             # wrapper
  data/content_shell_data/Cookies                 # persisted cookies
  data/content_shell_data/Cache/                  # HTTP disk cache
```

### Repo layout (host)

```
/root/chromium/src/                    # Chromium + Berry patches
/root/chromium/src/deploy/             # scripts, .bar sources, probes
/root/chromium/src/services/network/   # shims (url_loader.cc, qnx/*.js)
/root/berry-browser-status.md          # this file
/root/berry-agent-handoff-*.md         # feature/session handoffs
/root/toolchain-info.md                # compiler versions
```

### Adding a new build (checklist)

- [ ] Code change + `ninja -C out/qnx-arm content/shell:content_shell`  
- [ ] Bump `<buildId>` in `bar-descriptor-v3.xml`  
- [ ] Bump `build N` string in `launcher.c`  
- [ ] `./deploy/build-v3-bar.sh`  
- [ ] Deploy binary (+ launcher if changed) via root SSH  
- [ ] scp `.bar` to `shared/misc`  
- [ ] Verify build string in log  
- [ ] Update this file's build changelog (one line)  

---

## Deploy & build (quick reference)

### Build engine (~2 hours)

```bash
cd /root/chromium/src
ninja -C out/qnx-arm content/shell:content_shell
```

### Package `.bar`

```bash
cd /root/chromium/src
./deploy/build-v3-bar.sh
# → deploy/berry-shell-bar/BerryBrowserV3-3.0.2-build<N>.bar
```

### Fast deploy (engine only)

```bash
APP_NATIVE="/accounts/1000/appdata/com.sw7ft.BerryShellV3.testRel_erryShellV3d55e24f1/app/native"
scp out/qnx-arm/content_shell passport-root:${APP_NATIVE}/content_shell.exe.new
ssh passport-root "cd ${APP_NATIVE} && mv content_shell.exe.new content_shell.exe && chmod +x content_shell.exe"
# Also deploy launcher if build string changed
```

### Stage `.bar` for sideload

```bash
scp deploy/berry-shell-bar/BerryBrowserV3-3.0.2-build49.bar \
  passport-direct:/accounts/1000/shared/misc/
# Files app → tap → Install
```

### Pull logs

```bash
ssh passport 'tail -100 /accounts/1000/shared/misc/berry-kbd.log'
./chromium/src/deploy/pull-captcha-log.sh --live 120 /root/berry-session.log
grep -E 'SorryBounce|WatchShim|SearchShim|sorry|429|CRASH' /root/berry-session.log
```

---

## Device & SSH

| Host | Use |
|------|-----|
| `passport` | devuser @ port 2222 (USB bridge) |
| `passport-direct` | devuser @ 169.254.0.1 |
| `passport-root` | markers, logs, deploy to appdata |

Installed app ID: `com.sw7ft.BerryShellV3.testRel_erryShellV3d55e24f1`

---

## Shim disable markers

| Marker | Effect |
|--------|--------|
| `berry-youtube-shim.disable` | Off watch + search shims |
| `berry-google-sorry-shim.disable` | Off `/sorry` soft-bounce |
| `berry-decode.debug` | Log `DecodeProbe` per response |
| `berry-kbd.debug` / `berry-nav.debug` | Verbose nav logging |

Full list: `deploy/berry-shell-bar/BROWSER-SETTINGS.md`

---

## Key source files

| Area | Path |
|------|------|
| Watch shim JS | `services/network/qnx/watch_shim.js` |
| Search shim JS | `services/network/qnx/watch_shim_search.js` |
| Shim wiring | `services/network/url_loader.cc` |
| Header gen | `services/network/qnx/gen_shim_header.py` |
| Launcher / markers | `deploy/berry-shell-bar/launcher.c` |
| Bar packaging | `deploy/build-v3-bar.sh` |
| Cookie/cache (QNX) | `content/shell/browser/shell_content_browser_client.cc` |
| JS fingerprint shim | `content/shell/renderer/shell_render_frame_observer.cc` |
| Decode probe | `base/qnx_trace.h`, `url_loader.cc` |

---

## Open items (priority order)

1. **Facebook/Messenger login** — account needs Facebook verification (laptop showed "Please verify your Facebook account"). Test on build 67: verify on laptop, then log in at `www.facebook.com` on Passport (desktop UA since build 66), then Messenger tile  
2. **Video playback** — user reports issues; need specifics (site, symptom). Watch shim verified OK; Bing Videos browsed Aug 31 with no logged decode errors  
3. **Verify build 67 fixes on device** — Messenger no longer OOMs (`grep 'V8 javascript OOM' berry-kbd.log`); `QNX:MT stall` lines are sane; merged disable-features shows in log  
4. **Consent cold-start** — SOCS/CONSENT on fresh profile vs warm; may remove a block category  
5. **Optional:** `/youtubei/v1/next` related videos on watch shim  
6. **Not planned:** kevlar home, m.youtube.com, reCAPTCHA cure, BotGuard/JA3  

---

## Reference documents

| File | Topic |
|------|-------|
| `/root/berry-browser-status.md` | **This file** — master status |
| `/root/berry-agent-handoff-recaptcha-for-next-agent.md` | Google gate: prevention vs cure, build 49 spec |
| `/root/berry-captcha-build48-writeup.md` | Build 48 CAPTCHA session |
| `/root/berry-agent-handoff-google-captcha.md` | CAPTCHA strategic framing |
| `/root/berry-agent-handoff-build45-status.md` | Watch OK vs browse broken |
| `/root/berry-agent-handoff-build46-search-spec.md` | Search shim spec |
| `/root/chromium/src/deploy/GOOGLE_CAPTCHA_LOG.md` | Full CAPTCHA investigation (Jun 24) |
| `/root/chromium/src/deploy/CACHING_PERF_NOTES.md` | Cache, V8, WhatsApp perf |
| `/root/chromium/src/deploy/GPU_EGL_NOTES.md` | GPU / qnx_screen |
| `/root/chromium/src/deploy/berry-shell-bar/BROWSER-SETTINGS.md` | All marker files |
| `/root/chromium-for-bb10/docs/BAR-DEPLOYMENT.md` | BAR install gotchas |

---

## Session artifacts (logs)

| File | Contents |
|------|----------|
| `/root/berry-captcha-build48-session.log` | Filtered CAPTCHA lines |
| `/root/berry-captcha-build48-full.log` | Build 48 session |
| `/root/berry-build43-handoff.log` | Long watch-shim debug log |

---

## One-paragraph for the next person

BerryBrowserV3 build **67** on a BlackBerry Passport runs Chromium single-process with **three network shims** (watch, search, sorry soft-bounce). **Start here:** `/root/berry-browser-status.md` — diagnostics via `berry-kbd.log` + markers, build via `ninja` + `build-v3-bar.sh`, deploy via root SSH (`passport-root`) to `content_shell.exe`. YouTube playback/search work via innertube JSON; kevlar home and reCAPTCHA cure are out of scope. Google search is intermittent; build 49+ bounce handles `/sorry`. Current focus: speed, stability (V8 heap raised to 256MB after Messenger OOM), video playback, and completing Facebook/Messenger login (account needs verification — build 66 gave `www.facebook.com` desktop UA for the verify flow). **Instrumentation PGO is parked** (QNX link blocked). Verify code **and** device before trusting handoff docs.
