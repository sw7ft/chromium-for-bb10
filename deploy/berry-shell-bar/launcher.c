/*
 * BerryShell launcher for BlackBerry 10 (QNX 8 ARM).
 *
 * The BB10 Navigator launches this tiny native ELF as the app entry point.
 * Its only job is to set up the environment that content_shell expects when it
 * is run directly over SSH (see deploy/browse.sh) and then exec content_shell
 * in place:
 *   - resolve the app's native asset dir (where all payload lives)
 *   - point LD_LIBRARY_PATH at the bundled libs (libstdc++/libgcc_s/libm)
 *   - point QNX_CA_BUNDLE at the shipped root store for HTTPS
 *   - render on the physical display via the qnx_screen ozone backend
 *
 * The first argument, if present, is used as the start URL; otherwise we open
 * a built-in start page.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char* kRotation = "90";
static const char* kScaleFactor = "--force-device-scale-factor=1";
static const char* kSharedMisc = "/accounts/1000/shared/misc/";

static int marker_exists(const char* name) {
  char path[512];
  snprintf(path, sizeof(path), "%s%s", kSharedMisc, name);
  return access(path, F_OK) == 0;
}

/* Read the contents of a marker file in shared/misc into out (NUL-terminated,
 * trailing whitespace/newlines stripped). Returns the trimmed length, or 0 if
 * the file is missing/empty. Used to A/B V8 flags without a content_shell
 * rebuild: write the flag string into berry-jsflags and relaunch. */
static size_t read_marker_text(const char* name, char* out, size_t out_sz) {
  char path[512];
  snprintf(path, sizeof(path), "%s%s", kSharedMisc, name);
  FILE* f = fopen(path, "r");
  if (!f)
    return 0;
  size_t got = fread(out, 1, out_sz - 1, f);
  fclose(f);
  out[got] = '\0';
  while (got > 0 && (out[got - 1] == '\n' || out[got - 1] == '\r' ||
                     out[got - 1] == ' ' || out[got - 1] == '\t')) {
    out[--got] = '\0';
  }
  return got;
}

/* Load-reduction profiles for X.com crash triage (touch marker in shared/misc).
 * Profiles stack: berry-x-lite.enable implies 420 + 12fps + 2 raster threads.
 * Individual markers: berry-x-420.enable, berry-x-slow12.enable,
 * berry-x-slow15.enable. Logged at startup as BerryShell: load profile = ...
 * Remove all berry-x-* markers to restore default 720² / uncapped / 4 threads. */
static void apply_x_load_profile(int* render_w,
                                 int* render_h,
                                 int* max_fps,
                                 int* raster_threads,
                                 int* use_fps_limit_flag,
                                 char* profile_name,
                                 size_t profile_name_len) {
  strncpy(profile_name, "default(1440)", profile_name_len);

  if (marker_exists("berry-x-lite.enable")) {
    *render_w = 420;
    *render_h = 420;
    *max_fps = 12;
    *raster_threads = 2;
    *use_fps_limit_flag = 1;
    strncpy(profile_name, "lite(420+12fps+2thr)", profile_name_len);
    return;
  }
  if (marker_exists("berry-x-420.enable")) {
    *render_w = 420;
    *render_h = 420;
    strncpy(profile_name, "420", profile_name_len);
  }
  if (marker_exists("berry-x-540.enable")) {
    *render_w = 540;
    *render_h = 540;
    strncpy(profile_name, "540", profile_name_len);
  }
  if (marker_exists("berry-x-720.enable")) {
    *render_w = 720;
    *render_h = 720;
    strncpy(profile_name, "720", profile_name_len);
  }
  /* Native panel resolution: 1440² render with no downscale (sharpest, but the
   * heaviest -- 4x the pixels of 720). The output size below already tops out at
   * 1440, so render==output is 1:1. */
  if (marker_exists("berry-x-1440.enable")) {
    *render_w = 1440;
    *render_h = 1440;
    strncpy(profile_name, "1440", profile_name_len);
  }
  if (marker_exists("berry-x-slow10.enable")) {
    *max_fps = 10;
    *use_fps_limit_flag = 1;
    strncpy(profile_name, "slow10", profile_name_len);
  }
  if (marker_exists("berry-x-slow12.enable")) {
    *max_fps = 12;
    *use_fps_limit_flag = 1;
    strncpy(profile_name, "slow12", profile_name_len);
  }
  if (marker_exists("berry-x-slow15.enable")) {
    *max_fps = 15;
    *use_fps_limit_flag = 1;
    strncpy(profile_name, "slow15", profile_name_len);
  }
  if (marker_exists("berry-x-1thread.enable")) {
    *raster_threads = 1;
    strncat(profile_name, "+1thr", profile_name_len - strlen(profile_name) - 1);
  }
  if (marker_exists("berry-x-2thread.enable")) {
    *raster_threads = 2;
    strncat(profile_name, "+2thr", profile_name_len - strlen(profile_name) - 1);
  }
}

/* Stability: these networking/IPC features are documented (HARDENING.md) as
 * unstable on QNX — they deadlock or race the resource loader (the ~20-60s
 * crash on heavy sites like google.com was RawResource::NotifyFinished hitting
 * an invalid state via the dedicated network thread + sync cookie IPC). Viz is
 * intentionally left ENABLED because on-screen qnx_screen rendering needs it. */
static const char* kDisableFeatures =
    "--disable-features=ServiceWorker,NetworkServiceDedicatedThread,MojoIpcz,"
    "Translate,OptimizationHints,MediaRouter,PreconnectToSearch";

/* Same list but with ServiceWorker LEFT ENABLED. WhatsApp Web (and most PWAs)
 * use a Service Worker as their primary cache: it stores the app shell + JS +
 * WASM in Cache Storage so warm loads serve locally and bootstrap without
 * re-fetching. ServiceWorker was originally lumped in with the unstable
 * networking/IPC features above; re-enable it via the berry-sw.enable marker to
 * test whether it's stable here and how much it speeds repeat loads. */
static const char* kDisableFeaturesSW =
    "--disable-features=NetworkServiceDedicatedThread,MojoIpcz,"
    "Translate,OptimizationHints,MediaRouter,PreconnectToSearch";

/* Resolve the directory that holds this executable (the app's native asset
 * dir). The Navigator launches the app with cwd=/accounts/devuser (NOT the
 * asset dir), and argv[0] is not guaranteed to be an absolute path, so the
 * reliable source of truth on QNX is /proc/self/exefile, which yields the full
 * path of the running binary. Falls back to argv[0] then cwd. */
static void resolve_dir(char* dir, size_t n, const char* argv0) {
  dir[0] = '\0';

  int fd = open("/proc/self/exefile", O_RDONLY);
  if (fd >= 0) {
    ssize_t r = read(fd, dir, n - 1);
    close(fd);
    if (r > 0) {
      dir[r] = '\0';
      /* strip any trailing whitespace/newline */
      while (r > 0 && (dir[r - 1] == '\n' || dir[r - 1] == '\r' ||
                       dir[r - 1] == ' ' || dir[r - 1] == '\0'))
        dir[--r] = '\0';
    }
  }

  if (!dir[0] && argv0 && strchr(argv0, '/')) {
    strncpy(dir, argv0, n - 1);
    dir[n - 1] = '\0';
  }

  if (dir[0]) {
    char* slash = strrchr(dir, '/');
    if (slash)
      *slash = '\0';
  }

  if (!dir[0]) {
    if (!getcwd(dir, n))
      strncpy(dir, ".", n);
  }
}

int main(int argc, char** argv) {
  char dir[2048];
  resolve_dir(dir, sizeof(dir), argv[0]);

  /* content_shell derives its writable base (TMPDIR, shared memory, caches)
   * from getcwd() (see shell_main.cc). The Navigator launches the .bar with cwd
   * inside the READ-ONLY install image (/apps/<sandbox>/native), so if we leave
   * cwd there, Chromium cannot create its tmp dir / shared memory and
   * CHECK-crashes (SIGTRAP) ~15s in. Switch cwd to a WRITABLE sandbox dir:
   * prefer HOME (the BB10 sandbox's writable data dir), else compute
   * <sandbox>/data from the asset dir, else fall back to the asset dir. All
   * assets (libs, paks, home.html) are referenced by absolute path below, so
   * moving cwd off the asset dir is safe. */
  char work[2048];
  work[0] = '\0';
  {
    const char* home = getenv("HOME");
    if (home && home[0] && access(home, W_OK) == 0) {
      strncpy(work, home, sizeof(work) - 1);
      work[sizeof(work) - 1] = '\0';
    } else {
      strncpy(work, dir, sizeof(work) - 1);
      work[sizeof(work) - 1] = '\0';
      char* p = strstr(work, "/app/native");
      if (p) {
        *p = '\0';
        strncat(work, "/data", sizeof(work) - strlen(work) - 1);
      }
      if (access(work, W_OK) != 0) {
        strncpy(work, dir, sizeof(work) - 1);
        work[sizeof(work) - 1] = '\0';
      }
    }
  }
  if (work[0])
    chdir(work);

  /* Default to single-process (stable GPU + on-screen rendering). Multi-process
   * is experimental on QNX. Enable it either via QNX_MULTI_PROCESS=1 (SSH runs)
   * or by creating the marker file below — the Navigator UI launch can't set
   * env vars, and a UI launch is the ONLY context where BB10 hands the app a
   * working EGL display, so the marker lets us test MP from a real app launch
   * without reinstalling the .bar. Remove the marker to go back to SP. */
  const char* mp_env = getenv("QNX_MULTI_PROCESS");
  int use_multi_process = (mp_env && mp_env[0] == '1');
  if (!use_multi_process &&
      access("/accounts/1000/shared/misc/berry-mp.enable", F_OK) == 0)
    use_multi_process = 1;
  const int use_single_process = !use_multi_process;

  /* Software rendering is the default on Passport (A/B: faster steady-state than
   * EGL full-frame swap). Opt in to GPU: berry-gpu.enable or QNX_ENABLE_GPU=1.
   * berry-gpu.disable / QNX_DISABLE_GPU=1 still force software. */
  int use_gpu = 0;
  if (access("/accounts/1000/shared/misc/berry-gpu.enable", F_OK) == 0)
    use_gpu = 1;
  {
    const char* gpu_env = getenv("QNX_ENABLE_GPU");
    if (gpu_env && gpu_env[0] == '1')
      use_gpu = 1;
  }
  if (access("/accounts/1000/shared/misc/berry-gpu.disable", F_OK) == 0)
    use_gpu = 0;
  {
    const char* gpu_env = getenv("QNX_DISABLE_GPU");
    if (gpu_env && gpu_env[0] == '1')
      use_gpu = 0;
  }

  /* DIAGNOSTIC: the .bar runs sandboxed under a per-app uid, so its own data
   * dir and slog2 buffer are not readable over SSH as devuser. Redirect
   * content_shell's stdout+stderr to the SHARED folder (group-readable by
   * devuser) so we can capture startup/crash output and the QNX_KBD_DEBUG probe
   * from the actual app. Requires <access_shared> (declared in the descriptor).
   * Harmless if the open fails (e.g. permission) - the app still runs. */
  {
    /* Append, don't truncate: Navigator respawns the app on crash and was
     * wiping the only copy of the crash backtrace (O_TRUNC). */
    int lfd = open("/accounts/1000/shared/misc/berry-kbd.log",
                   O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (lfd >= 0) {
      dup2(lfd, 1);
      dup2(lfd, 2);
      if (lfd > 2)
        close(lfd);
    }
    /* Verbose touch/screen + nav IPC tracing costs measurable load time on BB10.
     * Enable only when debugging: touch berry-kbd.debug in shared/misc. */
    if (access("/accounts/1000/shared/misc/berry-kbd.debug", F_OK) == 0) {
      setenv("QNX_KBD_DEBUG", "1", 1);
      setenv("QNX_NAV_DEBUG", "1", 1);
    }
    if (access("/accounts/1000/shared/misc/berry-nav.debug", F_OK) == 0)
      setenv("QNX_NAV_DEBUG", "1", 1);
    /* Standalone GPU probe runs EGL init on the MAIN thread early in
     * ContentMain. Off by default (it leaves a stray EGL context current that
     * can confuse the real GLOzone GL init). Enable for diagnosis by creating
     * the berry-gpu.probe marker: lets us compare main-thread EGL init vs the
     * GPU/viz-thread GLOzone init in the same boot. */
    if (access("/accounts/1000/shared/misc/berry-gpu.probe", F_OK) == 0)
      setenv("QNX_GPU_PROBE", "1", 1);
    else
      setenv("QNX_GPU_PROBE", "0", 1);
  }

  fprintf(stderr, "BerryShell: app dir = %s\n", dir);
  fprintf(stderr, "BerryShell: work dir (cwd) = %s\n", work);
  fprintf(stderr, "BerryShell: %s\n",
          use_single_process ? "single-process mode" : "multi-process mode");
  fprintf(stderr, "BerryShell: %s (opt-in GPU: berry-gpu.enable in shared/misc)\n",
          use_gpu ? "GPU/EGL mode" : "software mode (--disable-gpu, default)");

  /* Bundled shared libs sit next to content_shell. */
  {
    char ld[4096];
    const char* old = getenv("LD_LIBRARY_PATH");
    snprintf(ld, sizeof(ld), "%s%s%s", dir, (old && old[0]) ? ":" : "",
             (old && old[0]) ? old : "");
    setenv("LD_LIBRARY_PATH", ld, 1);
  }

  /* HTTPS trust anchors shipped with the app (PEM bundle). */
  {
    char ca[2200];
    snprintf(ca, sizeof(ca), "%s/root_store.certs", dir);
    if (access(ca, R_OK) == 0)
      setenv("QNX_CA_BUNDLE", ca, 1);
  }

  /* Orientation correction for the Navigator-composited window (overridable). */
  setenv("QNX_SCREEN_ROTATION", kRotation, 0);

  /* Default render resolution is 1440² (native Passport panel, no downscale =
   * sharpest). Override with berry-x-420 / berry-x-720 / berry-x-1440 markers
   * (Settings > Display). 1440 is the heaviest (4x the pixels of 720). */
  int render_w = 1440, render_h = 1440;
  int max_fps = 0; /* 0 = no QNX_MAX_FPS env (Viz default 60 = uncapped) */
  int raster_threads = 4;
  int use_fps_limit_flag = 0; /* 0 => --disable-frame-rate-limit (current default) */
  char profile_name[64];
  apply_x_load_profile(&render_w, &render_h, &max_fps, &raster_threads,
                       &use_fps_limit_flag, profile_name, sizeof(profile_name));

  {
    char dim[16];
    snprintf(dim, sizeof(dim), "%d", render_w);
    setenv("QNX_SCREEN_WIDTH", dim, 1);
    snprintf(dim, sizeof(dim), "%d", render_h);
    setenv("QNX_SCREEN_HEIGHT", dim, 1);
  }
  /* Panel upscale stays 1440² so the window still fills the Passport display. */
  setenv("QNX_SCREEN_OUTPUT_WIDTH", "1440", 0);
  setenv("QNX_SCREEN_OUTPUT_HEIGHT", "1440", 0);
  if (max_fps > 0) {
    char fps[16];
    snprintf(fps, sizeof(fps), "%d", max_fps);
    setenv("QNX_MAX_FPS", fps, 1);
  }
  {
    char fps_buf[16];
    if (max_fps > 0)
      snprintf(fps_buf, sizeof(fps_buf), "%d", max_fps);
    fprintf(stderr,
            "BerryShell: load profile = %s render=%dx%d raster_thr=%d max_fps=%s "
            "fps_limit=%s\n",
            profile_name, render_w, render_h, raster_threads,
            max_fps > 0 ? fps_buf : "off",
            use_fps_limit_flag ? "on" : "off");
  }

  /* Exec the real binary directly. content_shell.bin is a legacy log wrapper that
   * only re-execs content_shell.exe; skipping it avoids an extra hop and ensures
   * deploy updates to content_shell.exe are what actually run. */
  char shell[2100];
  snprintf(shell, sizeof(shell), "%s/content_shell.exe", dir);

  /* Landing page resolution (highest priority first):
   *   1. berry-home-url (text marker): an explicit start URL. A bare host like
   *      "example.com" gets https:// prepended; anything with a scheme
   *      (http://, https://, file://) is used verbatim.
   *   2. berry-home.html in shared/misc: a user-editable landing page that
   *      overrides the bundled one without repackaging the .bar.
   *   3. bundled home.html in the native asset dir (default).
   * The first launcher argument still wins over all of these. */
  char default_url[2300];
  static char home_url_buf[2048];
  if (read_marker_text("berry-home-url", home_url_buf, sizeof(home_url_buf))) {
    if (strstr(home_url_buf, "://"))
      snprintf(default_url, sizeof(default_url), "%s", home_url_buf);
    else
      snprintf(default_url, sizeof(default_url), "https://%s", home_url_buf);
    fprintf(stderr, "BerryShell: landing = berry-home-url %s\n", default_url);
  } else if (access("/accounts/1000/shared/misc/berry-home.html", F_OK) == 0) {
    snprintf(default_url, sizeof(default_url),
             "file:///accounts/1000/shared/misc/berry-home.html");
    fprintf(stderr, "BerryShell: landing = custom berry-home.html\n");
  } else {
    snprintf(default_url, sizeof(default_url), "file://%s/home.html", dir);
    fprintf(stderr, "BerryShell: landing = bundled home.html\n");
  }
  const char* url = (argc > 1 && argv[1] && argv[1][0]) ? argv[1] : default_url;

  /* Mobile-first identity. We present as Android Chrome by default so sites
   * serve their (much lighter) mobile bundles -- the biggest load-time lever on
   * this Snapdragon 801 class hardware. This only sets the *default* UA; the
   * browser flips specific desktop-gated hosts (WhatsApp Web) to the desktop UA
   * per-navigation (see Shell::DidStartNavigation), which works no matter how
   * the user reaches them (omnibox, link, redirect). berry-desktop.enable forces
   * the desktop UA globally as an escape hatch. */
  int use_mobile_ua = !marker_exists("berry-desktop.enable");
  fprintf(stderr, "BerryShell: default ua = %s\n",
          use_mobile_ua ? "mobile" : "desktop");

  /* Custom user-agent override. Write the desired UA string into
   * shared/misc/berry-ua (single line) to spoof any device/browser. It wins
   * over the mobile/desktop default and suppresses --use-mobile-user-agent so
   * the requested string is sent verbatim. No content_shell rebuild needed. */
  static char ua_buf[1024];
  static char ua_arg[1056];
  int use_custom_ua = 0;
  if (read_marker_text("berry-ua", ua_buf, sizeof(ua_buf))) {
    snprintf(ua_arg, sizeof(ua_arg), "--user-agent=%s", ua_buf);
    use_custom_ua = 1;
    use_mobile_ua = 0;
    fprintf(stderr, "BerryShell: custom ua = %s\n", ua_buf);
  }

  char subprocess_path[2300];
  snprintf(subprocess_path, sizeof(subprocess_path),
           "--browser-subprocess-path=%s/content_shell.exe", dir);

  /* Single-process is the default and fastest path on BB10 (no cross-process
   * Mojo IPC for every navigation). Multi-process is experimental: it improves
   * crash isolation, not raw load speed. QNX has no zygote (keep --no-zygote)
   * and no sandbox (--no-sandbox). Set QNX_MULTI_PROCESS=1 or create
   * berry-mp.enable to try multi-process.
   *
   * Separate gpu-process on QNX currently spins/crashes (no shared Screen
   * context with the browser window). Keep GL in the browser via --in-process-gpu
   * while renderer/utility stay out-of-process.
   *
   * Children are launched by base::LaunchProcess (content/browser/
   * child_process_launcher_helper_qnx.cc), which already forwards LD_LIBRARY_PATH
   * and the rest of the env. Point them straight at content_shell.exe (the real
   * binary) rather than the content_shell.bin log-and-exec wrapper: the wrapper
   * does no env setup and only adds an extra execv hop that silently failed for
   * children (they spawned but never reached main()). */
  char* argv_buf[56];
  int n = 0;
  argv_buf[n++] = shell;
  argv_buf[n++] = (char*)"--no-sandbox";
  argv_buf[n++] = (char*)"--no-zygote";
  if (use_single_process)
    argv_buf[n++] = (char*)"--single-process";
  if (!use_single_process) {
    argv_buf[n++] = subprocess_path;
    argv_buf[n++] = (char*)"--in-process-gpu";
  }
  /* Startup / load-speed flags (safe on QNX; see deploy/HARDENING.md). */
  argv_buf[n++] = (char*)"--disable-background-networking";
  argv_buf[n++] = (char*)"--disable-background-timer-throttling";
  argv_buf[n++] = (char*)"--disable-renderer-backgrounding";
  argv_buf[n++] = (char*)"--disable-backgrounding-occluded-windows";
  argv_buf[n++] = (char*)"--disable-client-side-phishing-detection";
  argv_buf[n++] = (char*)"--disable-default-apps";
  argv_buf[n++] = (char*)"--disable-domain-reliability";
  argv_buf[n++] = (char*)"--disable-hang-monitor";
  argv_buf[n++] = (char*)"--disable-prompt-on-repost";
  argv_buf[n++] = (char*)"--disable-sync";
  argv_buf[n++] = (char*)"--disable-translate";
  argv_buf[n++] = (char*)"--no-first-run";
  argv_buf[n++] = (char*)"--no-default-browser-check";
  argv_buf[n++] = (char*)"--disable-component-update";
  /* Skip CertVerifierService Mojo round-trip on every HTTPS load (QNX has no
   * system trust store; full verify is slow and race-prone). TLS still encrypts;
   * only certificate validation is bypassed. See deploy/HARDENING.md. */
  argv_buf[n++] = (char*)"--ignore-certificate-errors";
  /* Auto-grant getUserMedia permission without a prompt: content_shell has no
   * permission UI, and this routes requests through the fake UI proxy instead
   * of the default WebContentsDelegate path (which is unimplemented on QNX and
   * would otherwise NOTREACHED/crash when e.g. X.com probes camera/mic at load).
   * --use-fake-device-for-media-stream is gated on a marker file so the real
   * QSA microphone (QsaInputStream) can be enabled per-device once the app
   * holds the BB10 record_audio permission; absent the marker we keep the fake
   * device so mic/camera probes never hit an unsupported real-capture path. */
  argv_buf[n++] = (char*)"--use-fake-ui-for-media-stream";
  if (access("/accounts/1000/shared/misc/berry-mic.enable", F_OK) != 0) {
    argv_buf[n++] = (char*)"--use-fake-device-for-media-stream";
  }
  /* NOTE: do NOT enable --remote-debugging-port here. It puts Chromium into
   * automation-controlled mode, which sets navigator.webdriver=true (a hard
   * bot signal that trips Google's secure-browser / reCAPTCHA checks) and
   * opens a CDP port. The renderer shim also forces navigator.webdriver=false,
   * but keeping this flag off removes the underlying signal entirely. */
  /* QUIC A/B: default off (historical stability), berry-quic.enable turns it on.
   * Now that DNS is reliable (built-in resolver), HTTP/3 0-RTT may cut connect
   * latency to Meta/Cloudflare CDNs. */
  if (!marker_exists("berry-quic.enable")) {
    argv_buf[n++] = (char*)"--disable-quic";
  } else {
    fprintf(stderr, "BerryShell: QUIC = enabled (berry-quic.enable)\n");
  }
  /* Telemetry blackhole A/B: berry-block.enable maps known non-essential Meta/
   * WhatsApp logging hosts to 0.0.0.0 so they fail fast instead of consuming
   * CPU, connections and DNS during the load-critical window. Conservative list
   * (crash-log upload only) so the chat UI is never affected. */
  if (marker_exists("berry-block.enable")) {
    argv_buf[n++] = (char*)"--host-resolver-rules=MAP crashlogs.whatsapp.net "
                           "0.0.0.0,MAP *.crashlogs.whatsapp.net 0.0.0.0";
    fprintf(stderr, "BerryShell: telemetry blocklist = on (berry-block.enable)\n");
  }
  /* No screen reader exists on BB10, so skip building and maintaining the
   * renderer accessibility tree. On heavy SPAs that tree is rebuilt on every
   * DOM mutation -- pure CPU we can hand back to the bootstrap JS. */
  argv_buf[n++] = (char*)"--disable-renderer-accessibility";
  if (!use_fps_limit_flag)
    argv_buf[n++] = (char*)"--disable-frame-rate-limit";
  argv_buf[n++] = (char*)"--ozone-platform=qnx_screen";
  {
    static char raster_flag[40];
    snprintf(raster_flag, sizeof(raster_flag), "--num-raster-threads=%d",
             raster_threads);
    if (use_gpu) {
      argv_buf[n++] = (char*)"--use-gl=egl";
      argv_buf[n++] = (char*)"--ignore-gpu-blocklist";
      /* Rasterize tiles on the Adreno 330 instead of CPU raster threads. The
       * GPU is otherwise idle during the JS-heavy load, and moving raster off
       * the Krait cores frees them for the single-threaded bootstrap JS that is
       * the real load bottleneck. */
      argv_buf[n++] = (char*)"--enable-gpu-rasterization";
      argv_buf[n++] = raster_flag;
    } else {
      argv_buf[n++] = (char*)"--disable-gpu";
      /* CRITICAL cold-start fix: --disable-gpu alone does NOT tell the renderer
       * that compositing is software-only (render_thread_impl.cc only sets
       * is_gpu_compositing_disabled_ for --disable-gpu-compositing). Without this,
       * the renderer's first LayerTreeFrameSink request still calls
       * EstablishGpuChannelSync, which blocks the main thread ~14s waiting for a
       * GPU channel that can never succeed (valid=0), serializing the first
       * navigation behind it. With this flag the renderer takes the software
       * frame-sink path immediately and skips the doomed GPU handshake. */
      argv_buf[n++] = (char*)"--disable-gpu-compositing";
      argv_buf[n++] = raster_flag;
    }
  }
  argv_buf[n++] = (char*)kScaleFactor;
  /* ServiceWorker A/B: berry-sw.enable keeps SW on (PWA app-shell caching);
   * default leaves it disabled for stability. */
  if (marker_exists("berry-sw.enable")) {
    argv_buf[n++] = (char*)kDisableFeaturesSW;
    fprintf(stderr, "BerryShell: ServiceWorker = ENABLED (berry-sw.enable)\n");
  } else {
    argv_buf[n++] = (char*)kDisableFeatures;
    fprintf(stderr, "BerryShell: ServiceWorker = disabled (default)\n");
  }
  /* Low-end device mode A/B: forces Chromium's phone memory/GC/tile heuristics
   * (smaller V8 heap, leaner image/tile caches). berry-lowend.enable to try. */
  if (marker_exists("berry-lowend.enable")) {
    argv_buf[n++] = (char*)"--enable-low-end-device-mode";
    fprintf(stderr, "BerryShell: low-end device mode = ON (berry-lowend.enable)\n");
  }
  /* Present as Android Chrome unless a desktop-gated host forced desktop above.
   * content_shell's GetUserAgent()/GetUserAgentMetadata() and OverrideWebkitPrefs
   * key off this switch to emit a mobile UA, Sec-CH-UA-Mobile: ?1, and mobile
   * viewport layout. */
  if (use_mobile_ua)
    argv_buf[n++] = (char*)"--use-mobile-user-agent";
  if (use_custom_ua)
    argv_buf[n++] = ua_arg;
  /* Optional V8 flag passthrough for tuning experiments (e.g. WASM compile
   * levers). Write the flag string into shared/misc/berry-jsflags and relaunch;
   * no content_shell rebuild needed. Example contents:
   *   --liftoff-only --wasm-lazy-validation --trace-wasm-compilation-times */
  {
    static char jsflags_buf[1024];
    static char jsflags_arg[1040];
    if (read_marker_text("berry-jsflags", jsflags_buf, sizeof(jsflags_buf))) {
      snprintf(jsflags_arg, sizeof(jsflags_arg), "--js-flags=%s", jsflags_buf);
      argv_buf[n++] = jsflags_arg;
      fprintf(stderr, "BerryShell: js-flags = %s\n", jsflags_buf);
    }
  }
  /* Content/render settings driven by .bar markers, no rebuild needed. These
   * map to Blink's built-in Settings via the canonical --blink-settings switch
   * (a comma-separated name=value list applied on top of WebPreferences), so
   * they survive the QNX OverrideWebkitPrefs pass:
   *   berry-noimages.enable -> imagesEnabled=false      (stop image fetch/decode)
   *   berry-nojs.enable     -> scriptEnabled=false      (disable JavaScript)
   *   berry-dark.enable     -> forceDarkModeEnabled=true (force dark rendering)
   * Combined into a single switch so multiple toggles can co-exist. */
  {
    static char blink_arg[256];
    char settings[224];
    settings[0] = '\0';
    if (marker_exists("berry-noimages.enable"))
      strncat(settings, "imagesEnabled=false,",
              sizeof(settings) - strlen(settings) - 1);
    if (marker_exists("berry-nojs.enable"))
      strncat(settings, "scriptEnabled=false,",
              sizeof(settings) - strlen(settings) - 1);
    if (marker_exists("berry-dark.enable"))
      strncat(settings, "forceDarkModeEnabled=true,",
              sizeof(settings) - strlen(settings) - 1);
    size_t sl = strlen(settings);
    if (sl > 0) {
      if (settings[sl - 1] == ',')
        settings[sl - 1] = '\0'; /* strip trailing comma */
      snprintf(blink_arg, sizeof(blink_arg), "--blink-settings=%s", settings);
      argv_buf[n++] = blink_arg;
      fprintf(stderr, "BerryShell: blink-settings = %s\n", settings);
    }
  }
  argv_buf[n++] = (char*)url;
  argv_buf[n++] = NULL;

  execv(shell, argv_buf);

  /* Only reached if exec fails. */
  perror("BerryShell: execv content_shell failed");
  return 1;
}
