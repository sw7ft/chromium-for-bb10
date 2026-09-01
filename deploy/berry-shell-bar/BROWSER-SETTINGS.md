# BerryBrowser settings

## In-app Settings page (recommended)

Tap the **gear (⚙)** on the landing page, or type **`berry.settings`** in the URL
bar. This opens a Settings screen rendered by the engine where you can toggle
everything with on/off switches — **device profile** (Passport/Q10/Z10/etc.),
resolution tier (420/**720 default**/1440), frame
rate (60/15/12/Lite), dark mode, block images, disable JS, ad/tracker block,
desktop site, custom user-agent, GPU (default on), low-end mode (default on),
Service Workers, QUIC, and the start page.

The page writes the marker files for you (web pages can't, but the engine can),
so no SSH or file manager is needed. Most settings are applied at startup, so tap
**Apply & Restart** when you're done. The page always reflects the real saved
state.

The sections below document the underlying marker files (set by the Settings
page, or by hand for scripting/advanced use).

---

All settings are also toggled by **marker files** in the device's shared misc dir:

```
/accounts/1000/shared/misc/
```

The launcher (`launcher.c`, shipped inside the `.bar`) reads these on every app
start and translates them into built-in Chromium switches — **no rebuild or
reinstall needed** to change a setting. Toggle, then relaunch the app.

```bash
# enable a setting
ssh passport "touch /accounts/1000/shared/misc/berry-nojs.enable"
# disable it again
ssh passport "rm -f /accounts/1000/shared/misc/berry-nojs.enable"
# a text setting (single line)
ssh passport "echo 'Mozilla/5.0 (iPhone ...)' > /accounts/1000/shared/misc/berry-ua"
```

The launcher logs what it applied to `/accounts/1000/shared/misc/berry-kbd.log`
(`BerryShell: blink-settings = ...`, `BerryShell: custom ua = ...`, etc.).

---

## Content & privacy

| Marker | Effect | Chromium switch |
|--------|--------|-----------------|
| `berry-noimages.enable` | Block all images (no fetch/decode) — big load + RAM win | `--blink-settings=imagesEnabled=false` |
| `berry-nojs.enable` | Disable JavaScript entirely | `--blink-settings=scriptEnabled=false` |
| `berry-adblock.disable` | Turn **off** the built-in ad/tracker blocker (on by default) | (env `BERRY_ADBLOCK=0`) |
| `berry-block.disable` | Turn **off** telemetry host blocklist (on by default) | `--host-resolver-rules=...` |
| `berry-privacy.enable` | Re-enable FedCM/privacy-sandbox APIs (disabled by default) | `--disable-features=...` (inverted) |

## Appearance

| Marker | Effect | Chromium switch |
|--------|--------|-----------------|
| `berry-dark.enable` | Force dark-mode rendering on all pages | `--blink-settings=forceDarkModeEnabled=true` |

## Restart & landing page

| Setting | How |
|--------|-----|
| **Restart browser** | Tap the **Restart** tile on the landing page (or type `berry.restart` in the URL bar). Relaunches the engine and re-reads every marker — this is how a **resolution change takes effect**. |
| Custom landing page (editable) | Put your own HTML at `/accounts/1000/shared/misc/berry-home.html`; it overrides the bundled page. |
| Custom start URL | `berry-home-url` (text file), e.g. `https://news.ycombinator.com` — a bare host gets `https://` added. Wins over `berry-home.html`. |

Resolution and most launch flags are read once at startup, so after changing
them tap **Restart** (or relaunch the app) to apply.

## Identity (User-Agent)

| Marker | Effect |
|--------|--------|
| `berry-ua` (text file) | Send this exact UA string. Wins over the default and suppresses the mobile-UA switch. |
| `berry-desktop.enable` | Use the built-in desktop UA (default is mobile/Android Chrome). |

Note: a custom `berry-ua` overrides `berry-desktop.enable`.

## Device profile

Pick the phone model in **Settings → Device**. This sets the physical panel
size, screen rotation, and default render aspect ratio so **touch coordinates
map correctly** and the window fills the display.

| Marker / setting | Effect |
|------------------|--------|
| `berry-device` (text file) | Device id: `passport`, `classic`, `q10`, `q5`, `z10`, `z30`, `z3`, `leap` |
| `berry-device-rotation` (text, optional) | Override rotation: `0`, `90`, `180`, or `270` |

Launcher env vars set from the profile:

| Env var | Purpose |
|---------|---------|
| `QNX_SCREEN_OUTPUT_WIDTH/HEIGHT` | Physical panel (touch mapping) |
| `QNX_SCREEN_WIDTH/HEIGHT` | Chromium render viewport |
| `QNX_SCREEN_ROTATION` | Navigator compositor correction |

Default render sizes per device (before resolution tier scaling):

| Device | Panel | Default render |
|--------|-------|----------------|
| Passport | 1440×1440 | 720×720 |
| Classic / Q10 / Q5 | 720×720 | 540×540 |
| Z10 | 768×1280 | 384×640 |
| Z30 / Z3 / Leap | 720×1280 | 360×640 |

Resolution tiers (420 / 540 / 720 / 1440) scale the device default
proportionally (720 = profile default, 1440 = 2×, 420 = lightweight).

## Performance & screen

| Marker | Effect |
|--------|--------|
| `berry-x-lite.enable` | Load-reduction profile: 420² render + 12 fps + 2 raster threads |
| `berry-x-420.enable` | 420² render (lightest, fastest) |
| `berry-x-540.enable` | 540² render (**default** when no res marker) |
| `berry-x-720.enable` | 720² render |
| `berry-x-1440.enable` | 1440² render = native panel, no downscale (sharpest, heaviest) |
| `berry-x-540.enable` | 540² render |
| `berry-x-fullfps.enable` | Uncapped frame rate (~60 fps) |
| `berry-x-slow10/12/15/45.enable` | Cap frame rate to 10/12/15/45 fps (**45 default**) |
| `berry-x-1thread.enable` / `berry-x-2thread.enable` | Raster thread count |
| `berry-lowend.disable` | Turn **off** low-end device mode (on by default) |
| `berry-gpu.disable` | Force software rendering (GPU/EGL **on by default**) |
| `berry-gpu.enable` | Legacy; redundant (GPU is default on) |
| `berry-lowend.enable` | Legacy; redundant (low-end is default on) |
| `berry-sw.disable` | Turn **off** Service Workers (on by default for YouTube/PWA caching) |
| `berry-sw.enable` | Legacy; redundant (SW is default on) |
| `berry-video.debug` | Verbose media logging (`--enable-logging=stderr --v=1`) |
| `berry-quic.enable` | Enable HTTP/3 (QUIC) |
| `berry-mp.enable` | Multi-process mode (experimental) |
| `berry-jsflags` (text file) | Extra V8 flags, e.g. `--liftoff-only --wasm-lazy-validation` |

## Debug

| Marker | Effect |
|--------|--------|
| `berry-kbd.debug` | Verbose touch/keyboard + nav IPC tracing |
| `berry-nav.debug` | Navigation IPC tracing only |
| `berry-gpu.probe` | Standalone EGL init probe at startup |
| `berry-mic.enable` | Use the real QSA microphone instead of the fake media device |

---

## Combining settings

Multiple content/appearance toggles combine into one switch automatically, e.g.
`berry-noimages.enable` + `berry-nojs.enable` + `berry-dark.enable` →
`--blink-settings=imagesEnabled=false,scriptEnabled=false,forceDarkModeEnabled=true`.

A handy "data saver / fast" preset:

```bash
ssh passport "cd /accounts/1000/shared/misc && touch berry-noimages.enable berry-x-lite.enable berry-lowend.enable"
```
