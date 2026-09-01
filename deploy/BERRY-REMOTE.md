# Berry Remote — browser-based remote view + control of the BB10/QNX device

A lightweight "VNC/RDP-like" remote session for the BlackBerry Passport, viewable
and controllable from any desktop browser. No app install on the viewer side, no
plugins.

## What it actually does (and its one limitation)

It mirrors and controls a **Chromium `content_shell` instance running on the
device**, using Chromium's built-in remote-debugging (CDP):

- `Page.startScreencast` streams live JPEG frames of the page → relayed to the
  browser as an **MJPEG stream** (a plain `<img>` shows a live, auto-updating
  picture).
- The browser sends **mouse / scroll / keyboard / navigation** back over HTTP →
  injected into the page via CDP `Input.dispatch*` / `Page.navigate`.

It does **not** mirror the whole device desktop / other apps. Whole-screen capture
on BB10 is the root-only `snapshot` tool (and the privileged Screen capture API),
and we only have `devuser` over SSH — so a true full-device VNC isn't reachable
without device root. CDP needs no privilege and gives view+control of the browser
engine, which is the part we care about (WhatsApp / any web app). The remote
session is a *separate* Chromium from the installed `.bar` apps, with its own
profile (so you link/log in to it independently).

## Components

- `berry-remote.js` — the dependency-free Node service (CDP-pipe client + MJPEG
  fan-out + input endpoints + inline browser UI).
- `remote-start.sh` / `remote-stop.sh` — start/stop helpers. Persistence without
  `nohup`/`setsid` (absent on the device busybox) is done with `trap '' HUP`,
  whose ignored-SIGHUP disposition is inherited by node across fork/exec, so the
  service survives the launching SSH session closing.
- Runs from `/accounts/devuser/berry-deploy/berry-browser-bundle/` on the device
  (which holds `content_shell` + paks + libs + `root_store.certs`), using the
  device Node at `/accounts/devuser/berry-deploy/node/node`.

## Run it

On the device (over SSH):

```sh
cd /accounts/devuser/berry-deploy/berry-browser-bundle
PORT=8080 URL='https://web.whatsapp.com/' sh remote-start.sh   # start
sh remote-stop.sh                                              # stop
```

Env knobs: `PORT` (default 8080), `URL` (default web.whatsapp.com), `RENDER`
(default 720), `QUALITY` (JPEG quality, default 60).

Key requirement: the service sets `QNX_DEVTOOLS=1` for the spawned engine —
content_shell gates DevTools/CDP behind that env on QNX (see
`shell_browser_main_parts.cc`; `StartHttpHandler` also starts the
`--remote-debugging-pipe` handler). Without it, CDP never starts.

## Open it in a browser

The service binds `0.0.0.0:PORT`. Two ways to reach it:

1. **Same LAN as the device** (device is at `192.168.1.107`):
   open `http://192.168.1.107:8080`.

2. **Via SSH port-forward** (works from anywhere you can SSH to the device):

   ```sh
   ssh -L 8080:127.0.0.1:8080 passport
   ```
   then open `http://localhost:8080`.

In the page: click / drag / scroll directly on the screen image; use the URL bar
to navigate; click **Keyboard** to focus a hidden field and type into the page.

## Endpoints (for scripting)

- `GET /` — the viewer UI
- `GET /stream` — MJPEG (`multipart/x-mixed-replace`)
- `POST /input` — `{kind:'down|up|move|wheel', fx, fy, dx, dy}` (fx/fy normalized 0..1)
- `POST /key` — `{type:'keyDown|keyUp|char', key, code, keyCode, text}`
- `POST /nav` — `{url}`
- `GET /status` — JSON `{session, frames, clients, meta}`
