/*
 * BerryWhatsApp launcher for BlackBerry 10 (QNX 8 ARM).
 *
 * A single-purpose "web app" wrapper: it boots Chromium content_shell straight
 * into https://web.whatsapp.com/ with a fixed, WhatsApp-tuned configuration.
 * Unlike the general BerryBrowser launcher this one is deliberately SIMPLE --
 * there are no shared/misc setting markers to read, so the WhatsApp app behaves
 * identically on every launch and stays fully isolated from the main browser
 * (each .bar gets its own sandbox $HOME, hence its own persistent
 * content_shell_data: the WhatsApp QR pairing / IndexedDB survives restarts and
 * is never touched by BerryBrowser).
 *
 * WhatsApp-specific choices (vs. the general launcher's defaults):
 *   - start URL hardcoded to web.whatsapp.com (argv[1] still overrides, for SSH)
 *   - DESKTOP user-agent from the very first request (WhatsApp Web refuses the
 *     mobile UA with "open WhatsApp on your phone"); content_shell additionally
 *     pins the desktop UA for *.whatsapp.com per-navigation.
 *   - Service Worker ENABLED: WhatsApp's app shell + JS + media live in its
 *     Cache Storage service worker, so warm launches bootstrap locally.
 *   - real microphone ENABLED (voice messages) -- the app declares record_audio.
 *   - 720x720 render (good sharpness/speed balance for the chat UI) upscaled to
 *     the 1440 Passport panel.
 *   - non-essential WhatsApp crash-log host black-holed so it never competes for
 *     CPU/DNS/sockets during the load-critical window.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char* kStartUrl = "https://web.whatsapp.com/";
static const char* kRotation = "90";
static const char* kLogPath = "/accounts/1000/shared/misc/whatsapp-app.log";

/* ServiceWorker LEFT ENABLED (absent from the list). The other features are the
 * documented-unstable networking/IPC ones that deadlock the resource loader on
 * QNX (see deploy/HARDENING.md); Viz stays enabled for on-screen rendering. */
static const char* kDisableFeatures =
    "--disable-features=NetworkServiceDedicatedThread,MojoIpcz,"
    "Translate,OptimizationHints,MediaRouter,PreconnectToSearch";

/* Resolve the directory holding this executable (the app's native asset dir).
 * The Navigator launches with cwd=/accounts/devuser and a non-absolute argv[0],
 * so /proc/self/exefile is the reliable source of truth; fall back to argv[0]
 * then cwd. */
static void resolve_dir(char* dir, size_t n, const char* argv0) {
  dir[0] = '\0';

  int fd = open("/proc/self/exefile", O_RDONLY);
  if (fd >= 0) {
    ssize_t r = read(fd, dir, n - 1);
    close(fd);
    if (r > 0) {
      dir[r] = '\0';
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

  /* content_shell derives its writable base (TMPDIR, shared memory, caches, and
   * the persistent profile $HOME/content_shell_data) from cwd. The Navigator
   * launches the .bar with cwd inside the READ-ONLY install image, so move cwd
   * to the writable sandbox dir: prefer HOME, else <sandbox>/data, else the
   * asset dir. All assets are referenced by absolute path below. */
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

  /* Sandboxed under a per-app uid, so slog2 isn't readable over SSH as devuser.
   * Mirror stdout/stderr into the shared folder (needs <access_shared>) so the
   * boot log is capturable. Append (Navigator respawns on crash). Harmless if
   * the open fails. */
  {
    int lfd = open(kLogPath, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (lfd >= 0) {
      dup2(lfd, 1);
      dup2(lfd, 2);
      if (lfd > 2)
        close(lfd);
    }
  }

  fprintf(stderr, "BerryWhatsApp: app dir = %s\n", dir);
  fprintf(stderr, "BerryWhatsApp: work dir (cwd) = %s\n", work);

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

  /* Orientation correction for the Navigator-composited window. */
  setenv("QNX_SCREEN_ROTATION", kRotation, 0);

  /* App mode: tell the engine to skip the BerryBrowserChrome URL toolbar so
   * WhatsApp Web renders full-screen like a native app (the page gets the whole
   * window). WhatsApp auto-reconnects its websocket, so the toolbar's
   * reload/back controls aren't needed for a single-site app. */
  setenv("BERRY_APP_MODE", "1", 1);

  /* 720x720 render, upscaled to fill the 1440 Passport panel. */
  setenv("QNX_SCREEN_WIDTH", "720", 1);
  setenv("QNX_SCREEN_HEIGHT", "720", 1);
  setenv("QNX_SCREEN_OUTPUT_WIDTH", "1440", 0);
  setenv("QNX_SCREEN_OUTPUT_HEIGHT", "1440", 0);

  char shell[2100];
  snprintf(shell, sizeof(shell), "%s/content_shell.exe", dir);

  const char* url = (argc > 1 && argv[1] && argv[1][0]) ? argv[1] : kStartUrl;
  fprintf(stderr, "BerryWhatsApp: url = %s\n", url);

  /* Single-process software rendering is the proven-stable path on BB10. */
  char* a[48];
  int n = 0;
  a[n++] = shell;
  a[n++] = (char*)"--no-sandbox";
  a[n++] = (char*)"--no-zygote";
  a[n++] = (char*)"--single-process";
  /* Startup / load-speed flags (safe on QNX; see deploy/HARDENING.md). */
  a[n++] = (char*)"--disable-background-networking";
  a[n++] = (char*)"--disable-background-timer-throttling";
  a[n++] = (char*)"--disable-renderer-backgrounding";
  a[n++] = (char*)"--disable-backgrounding-occluded-windows";
  a[n++] = (char*)"--disable-client-side-phishing-detection";
  a[n++] = (char*)"--disable-default-apps";
  a[n++] = (char*)"--disable-domain-reliability";
  a[n++] = (char*)"--disable-hang-monitor";
  a[n++] = (char*)"--disable-prompt-on-repost";
  a[n++] = (char*)"--disable-sync";
  a[n++] = (char*)"--disable-translate";
  a[n++] = (char*)"--no-first-run";
  a[n++] = (char*)"--no-default-browser-check";
  a[n++] = (char*)"--disable-component-update";
  /* Pin a generous on-disk HTTP cache (256 MB). The engine already persists the
   * HTTP cache + V8 GeneratedCodeCache under the per-app profile, but the size
   * defaults to a disk-space heuristic; pinning it guarantees WhatsApp's
   * multi-MB JS/WASM bundle + static assets stay resident across launches
   * instead of being evicted, so warm starts serve locally instead of
   * re-downloading. (~23 GB free on the device; 256 MB is comfortable.) */
  a[n++] = (char*)"--disk-cache-size=268435456";
  /* QNX has no system trust store; full verify is slow/race-prone. TLS still
   * encrypts -- only certificate validation is bypassed. See HARDENING.md. */
  a[n++] = (char*)"--ignore-certificate-errors";
  /* Auto-grant getUserMedia (no permission UI in content_shell) and route mic
   * through the fake UI proxy. The real QSA microphone (QsaInputStream) is left
   * ENABLED (no --use-fake-device-for-media-stream) so WhatsApp voice messages
   * capture real audio; the app declares record_audio in its descriptor. */
  a[n++] = (char*)"--use-fake-ui-for-media-stream";
  a[n++] = (char*)"--disable-quic";
  /* Black-hole WhatsApp's non-essential crash-log host so it fails fast instead
   * of consuming CPU/DNS/sockets during the load-critical window. The chat UI
   * never talks to these hosts. */
  a[n++] = (char*)"--host-resolver-rules=MAP crashlogs.whatsapp.net 0.0.0.0,"
                  "MAP *.crashlogs.whatsapp.net 0.0.0.0";
  /* No screen reader on BB10: skip the renderer accessibility tree (rebuilt on
   * every DOM mutation on heavy SPAs -- pure CPU we hand back to WhatsApp's JS). */
  a[n++] = (char*)"--disable-renderer-accessibility";
  a[n++] = (char*)"--disable-frame-rate-limit";
  a[n++] = (char*)"--ozone-platform=qnx_screen";
  a[n++] = (char*)"--disable-gpu";
  /* CRITICAL cold-start fix: tell the renderer compositing is software-only so
   * its first LayerTreeFrameSink takes the software path instead of blocking
   * ~14s on a GPU channel that can never succeed. See launcher.c. */
  a[n++] = (char*)"--disable-gpu-compositing";
  a[n++] = (char*)"--num-raster-threads=4";
  a[n++] = (char*)"--force-device-scale-factor=1";
  /* ServiceWorker ON for WhatsApp's app-shell cache. */
  a[n++] = (char*)kDisableFeatures;
  /* WASM tuning for the Krait: WhatsApp ships heavy WASM (Signal protocol /
   * crypto). Default V8 baseline-compiles with Liftoff then RE-compiles hot
   * functions with the optimizing TurboFan tier on background threads -- that
   * tier-up steals all four cores during/after the bootstrap, which is exactly
   * when the main thread needs them, so the app sits pegged while loading.
   * Disable tier-up (Liftoff-only) and skip eager module validation: far less
   * compile CPU at load, at a small steady-state crypto cost that is invisible
   * for chat-volume traffic. */
  a[n++] = (char*)"--js-flags=--no-wasm-tier-up --wasm-lazy-validation";
  /* DESKTOP UA: do NOT pass --use-mobile-user-agent. content_shell's default UA
   * is desktop Chrome 120, and it additionally pins the desktop UA for
   * *.whatsapp.com per-navigation (Shell::DidStartNavigation). */
  a[n++] = (char*)url;
  a[n++] = NULL;

  fprintf(stderr, "BerryWhatsApp: launching content_shell (SW on, desktop UA, "
                  "real mic, 720)\n");

  execv(shell, a);

  perror("BerryWhatsApp: execv content_shell failed");
  return 1;
}
