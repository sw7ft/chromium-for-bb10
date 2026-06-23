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

/* Stability: these networking/IPC features are documented (HARDENING.md) as
 * unstable on QNX — they deadlock or race the resource loader (the ~20-60s
 * crash on heavy sites like google.com was RawResource::NotifyFinished hitting
 * an invalid state via the dedicated network thread + sync cookie IPC). Viz is
 * intentionally left ENABLED because on-screen qnx_screen rendering needs it. */
static const char* kDisableFeatures =
    "--disable-features=ServiceWorker,NetworkServiceDedicatedThread,MojoIpcz,"
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
    int lfd = open("/accounts/1000/shared/misc/berry-kbd.log",
                   O_WRONLY | O_CREAT | O_TRUNC, 0644);
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
    /* Probe already confirmed Adreno 330 / EGL 1.4 / GLES2 works in-app; the
     * real GLOzone path now drives GL, so leave the standalone probe off to
     * avoid leaving a stray EGL context current before content GL init. */
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
  /* Render/composit at 720² (~4× fewer pixels than native 1440²); panel upscale
   * to full screen via QNX Screen SIZE/SOURCE_SIZE in qnx_screen_window.cc. */
  setenv("QNX_SCREEN_WIDTH", "720", 0);
  setenv("QNX_SCREEN_HEIGHT", "720", 0);
  setenv("QNX_SCREEN_OUTPUT_WIDTH", "1440", 0);
  setenv("QNX_SCREEN_OUTPUT_HEIGHT", "1440", 0);

  /* Exec the real binary directly. content_shell.bin is a legacy log wrapper that
   * only re-execs content_shell.exe; skipping it avoids an extra hop and ensures
   * deploy updates to content_shell.exe are what actually run. */
  char shell[2100];
  snprintf(shell, sizeof(shell), "%s/content_shell.exe", dir);

  /* Default start URL: bundled home.html in the native asset dir (omnibox +
   * quick links). Override with the first launcher argument. */
  char default_url[2300];
  snprintf(default_url, sizeof(default_url), "file://%s/home.html", dir);
  const char* url = (argc > 1 && argv[1] && argv[1][0]) ? argv[1] : default_url;

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
  char* argv_buf[32];
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
  argv_buf[n++] = (char*)"--remote-debugging-port=0";
  argv_buf[n++] = (char*)"--disable-quic";
  argv_buf[n++] = (char*)"--disable-frame-rate-limit";
  argv_buf[n++] = (char*)"--ozone-platform=qnx_screen";
  if (use_gpu) {
    argv_buf[n++] = (char*)"--use-gl=egl";
    argv_buf[n++] = (char*)"--ignore-gpu-blocklist";
    argv_buf[n++] = (char*)"--num-raster-threads=4";
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
    /* Software Skia raster: use all 4 cores on Passport (was implicit 1). */
    argv_buf[n++] = (char*)"--num-raster-threads=4";
  }
  argv_buf[n++] = (char*)kScaleFactor;
  argv_buf[n++] = (char*)kDisableFeatures;
  argv_buf[n++] = (char*)url;
  argv_buf[n++] = NULL;

  execv(shell, argv_buf);

  /* Only reached if exec fails. */
  perror("BerryShell: execv content_shell failed");
  return 1;
}
