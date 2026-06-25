# GPU / EGL on BB10 (QNX, Adreno 330) — read before testing GPU

## TL;DR — the trap that cost us hours

**EGL only initializes when the app is launched by TAPPING the icon (a real
Navigator app session). It does NOT initialize when content_shell is launched
over SSH.** This is not a code bug and not "wedged GPU state" — it is how BB10
graphics works: `eglGetDisplay(EGL_DEFAULT_DISPLAY)` needs the window-group /
graphics session that the Navigator gives a foreground app.

If you launch GPU mode over SSH you will ALWAYS see this and wrongly conclude
GPU is broken:

```
QNX GL: native EGL/GLES2 bindings loaded
QNX GL: GetNativeDisplay using EGL_DEFAULT_DISPLAY (screen_context=...)
QNX GL PROBE: bindAPI=0 getDisplay=0 (err=0x3001) initialize=0 v0.0 (err=0x3001)
ERROR gl_display.cc: Initialization of all EGL display types failed.
ERROR viz_main_impl.cc: Exiting GPU process due to errors during initialization
```

`0x3001 = EGL_NOT_INITIALIZED`, `getDisplay=0` = `EGL_NO_DISPLAY`. The libs
loaded fine and `screen_create_context` succeeded — only the EGL↔display-server
handshake fails, because there is no app graphics session over SSH.

When launched by TAPPING the icon, the SAME build/code works:

```
QNX GL PROBE: getDisplay=77f24ef4 (err=0x3000) initialize=1 v1.4 (err=0x3000)
gpu_init.cc: Vulkan not supported with in process gpu      <-- NON-FATAL, ignore
BerryNav: GpuChReqSend ... GpuChReply tid=7 valid=1         <-- GPU channel OK
BerryNav: DidFinishNavigation ... home.html error=0         <-- page committed
alive_exe=33                                                <-- process healthy
```

`0x3000 = EGL_SUCCESS`, `getDisplay` is a real handle, `eglInitialize` returns
EGL 1.4. The "Vulkan not supported with in process gpu" line is expected and
harmless (Chromium just skips the Vulkan probe under `--single-process` /
in-process GPU; it falls back to GLES2/EGL, which is what we force anyway).

## How software vs GPU differ over SSH

- **Software** (`--disable-gpu`, default) renders fine over SSH because it only
  needs `screen_post_window` (a plain Screen buffer post) — no GPU display.
  Look for `QNX:FPS present=N` (software-path present counter).
- **GPU/EGL** needs a full GPU display from the graphics service → requires the
  tapped app session. There is NO `QNX:FPS present` log in GPU mode; presentation
  goes through `eglSwapBuffers`, so don't use that counter to judge GPU.

## Enable / disable GPU (no rebuild — launcher marker files)

`deploy/berry-shell-bar/launcher.c` reads markers in `/accounts/1000/shared/misc`:

```bash
# Enable GPU (then TAP the icon to launch — not SSH):
ssh passport 'touch /accounts/1000/shared/misc/berry-gpu.enable'

# Back to software (default / safe):
ssh passport 'rm -f /accounts/1000/shared/misc/berry-gpu.enable'
# (berry-gpu.disable also force-software even if enable exists)
```

`QNX_ENABLE_GPU=1` / `QNX_DISABLE_GPU=1` env vars do the same.

## Diagnostics left in the tree (GPU-path only, harmless in software mode)

- **`berry-gpu.probe` marker** → launcher sets `QNX_GPU_PROBE=1`, which runs the
  standalone main-thread EGL probe (`gpu/qnx/gpu_qnx.cc`) early in
  `content/shell/app/shell_main.cc`. Useful to compare main-thread EGL init vs
  the GPU/viz-thread GLOzone init in one boot. Off by default (it leaves a stray
  EGL context current that can confuse real GL init).
- **`QNX GL PROBE:` log line** in
  `ui/ozone/platform/qnx_screen/qnx_screen_gl_ozone_egl.cc`
  `GetNativeDisplay()` — calls `eglGetDisplay`/`eglInitialize` directly and prints
  `eglGetError` so we always see the real EGL error code (the stock gl stack only
  logs the generic "Initialization of all EGL display types failed").

## Environment facts (device, verified)

- `/usr/lib/libEGL.so -> libEGL.so.1`, `libGLESv2.so -> .so.1`,
  `libscreen.so -> .so.1` all present. The custom loader dlopens the UNVERSIONED
  names with `RTLD_GLOBAL` (libEGL first) — correct; the stock loader's
  `libGLESv2.so.2` is wrong for this device.
- Adreno driver backends present: `/usr/lib/graphics/{msm8960,qc}/egl14.so`,
  `OpenGLES20.so`, `eglsub-screen.so`.
- GPU path forces an **ES2** context (`CreateGLContext`) because the Adreno 330
  driver advertises ES2 but exports null ES3 entry points that crash if probed.
- `SupportsPostSubBuffer()` is forced **false**: BB10 EGL advertises
  `EGL_NV_post_sub_buffer` but partial GL present corrupts the window, so the
  compositor must full-frame swap every frame.

## Testing checklist (so we don't repeat the SSH mistake)

1. `touch berry-gpu.enable` on device.
2. **TAP the Berry Browser icon** (do NOT `ssh ... $APP/launcher`).
3. Read `/accounts/1000/shared/misc/berry-kbd.log`; expect
   `QNX GL PROBE: ... initialize=1 v1.4` and `GpuChReply ... valid=1`.
4. Judge GPU rendering by what is ON SCREEN (and `eglSwapBuffers` / `QNX:BF
   needs_draw=1`), NOT by `QNX:FPS present` (software-only counter).
5. To revert: `rm -f berry-gpu.enable` and relaunch (software).

## Why software is still the DEFAULT

Performance, not capability. Steady-state UI is faster in software on this SoC
because the GPU path pays full-frame `eglSwapBuffers` every frame (PostSubBuffer
disabled for correctness) plus Viz/GL-thread overhead. See
`gpu_vs_software_analysis` plan. GPU mode is real and now confirmed to initialize
on a tapped launch; keep it opt-in.
