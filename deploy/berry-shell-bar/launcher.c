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
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <screen/screen.h>

static const char* kScaleFactor = "--force-device-scale-factor=1";
static const char* kSharedMisc = "/accounts/1000/shared/misc/";

static int marker_exists(const char* name);
static size_t read_marker_text(const char* name, char* out, size_t out_sz);
static void scale_render_dims(int* render_w,
                              int* render_h,
                              int base_w,
                              int base_h,
                              float scale);

/* BB10 device panel geometry (portrait). Render defaults are ~half panel on the
 * short axis unless the user picks a Display resolution tier. Touch coordinates
 * from libscreen are mapped using QNX_SCREEN_OUTPUT_* vs render size. */
typedef struct {
  const char* id;
  const char* label;
  int output_w;
  int output_h;
  int rotation;
  int default_render_w;
  int default_render_h;
} BerryDeviceProfile;

static const BerryDeviceProfile kDeviceProfiles[] = {
    {"passport", "Passport", 1440, 1440, 90, 720, 720},
    {"classic", "Classic", 720, 720, 90, 540, 540},
    {"q20", "Classic", 720, 720, 90, 540, 540},
    {"q10", "Q10", 720, 720, 90, 540, 540},
    {"q5", "Q5", 720, 720, 90, 540, 540},
    {"z10", "Z10", 768, 1280, 90, 384, 640},
    {"z30", "Z30", 720, 1280, 90, 360, 640},
    {"z3", "Z3", 720, 1280, 90, 360, 640},
    {"leap", "Leap", 720, 1280, 90, 360, 640},
};

static const BerryDeviceProfile* find_device_profile(const char* id) {
  if (id && id[0]) {
    for (size_t i = 0; i < sizeof(kDeviceProfiles) / sizeof(kDeviceProfiles[0]);
         ++i) {
      if (strcmp(id, kDeviceProfiles[i].id) == 0)
        return &kDeviceProfiles[i];
    }
  }
  return NULL;
}

/* Match a physical panel size (either orientation) to a known profile. */
static const BerryDeviceProfile* find_profile_by_panel(int w, int h) {
  for (size_t i = 0; i < sizeof(kDeviceProfiles) / sizeof(kDeviceProfiles[0]);
       ++i) {
    const BerryDeviceProfile* p = &kDeviceProfiles[i];
    if ((p->output_w == w && p->output_h == h) ||
        (p->output_w == h && p->output_h == w))
      return p;
  }
  return NULL;
}

/* Ask libscreen for the native panel size. This is what fixes touch
 * calibration on Q10/Q20/Classic etc. without the user picking a device in
 * Settings: touch events arrive in PANEL coordinates and are mapped to the
 * render surface via QNX_SCREEN_OUTPUT_* (see qnx_screen_sizes.h), so a wrong
 * output size (Passport 1440 assumed on a 720 panel) puts every tap at half
 * position. Returns 1 on success. */
static int detect_panel_size(int* w, int* h) {
  screen_context_t ctx = NULL;
  screen_display_t displays[8];
  int count = 0;
  int size[2] = {0, 0};
  int ok = 0;
  if (screen_create_context(&ctx, SCREEN_APPLICATION_CONTEXT) != 0)
    return 0;
  if (screen_get_context_property_iv(ctx, SCREEN_PROPERTY_DISPLAY_COUNT,
                                     &count) == 0 &&
      count > 0) {
    if (count > 8)
      count = 8;
    memset(displays, 0, sizeof(displays));
    if (screen_get_context_property_pv(ctx, SCREEN_PROPERTY_DISPLAYS,
                                       (void**)displays) == 0 &&
        displays[0] &&
        screen_get_display_property_iv(displays[0], SCREEN_PROPERTY_SIZE,
                                       size) == 0 &&
        size[0] > 0 && size[1] > 0) {
      *w = size[0];
      *h = size[1];
      ok = 1;
    }
  }
  screen_destroy_context(ctx);
  return ok;
}

static void apply_device_profile(int* output_w,
                                 int* output_h,
                                 int* rotation,
                                 int* base_render_w,
                                 int* base_render_h,
                                 char* device_name,
                                 size_t device_name_len) {
  /* Passport fallback if both the marker and detection fail. */
  const BerryDeviceProfile* p = &kDeviceProfiles[0];
  char dev_id[64];
  int have_marker = read_marker_text("berry-device", dev_id, sizeof(dev_id)) &&
                    strcmp(dev_id, "auto") != 0;

  if (have_marker) {
    const BerryDeviceProfile* m = find_device_profile(dev_id);
    if (m) {
      p = m;
      snprintf(device_name, device_name_len, "%s", p->label);
    } else {
      have_marker = 0; /* unknown id -> fall through to autodetect */
    }
  }
  if (!have_marker) {
    int det_w = 0, det_h = 0;
    if (detect_panel_size(&det_w, &det_h)) {
      const BerryDeviceProfile* m = find_profile_by_panel(det_w, det_h);
      if (m) {
        p = m;
        snprintf(device_name, device_name_len, "%s (auto)", p->label);
      } else {
        /* Unknown panel: use detected size directly, render at half. */
        *output_w = det_w;
        *output_h = det_h;
        *rotation = 90;
        scale_render_dims(base_render_w, base_render_h, det_w, det_h, 0.5f);
        snprintf(device_name, device_name_len, "auto %dx%d", det_w, det_h);
        char rot_buf2[16];
        if (read_marker_text("berry-device-rotation", rot_buf2,
                             sizeof(rot_buf2))) {
          int r = atoi(rot_buf2);
          if (r == 0 || r == 90 || r == 180 || r == 270)
            *rotation = r;
        }
        return;
      }
    } else {
      snprintf(device_name, device_name_len, "%s (default)", p->label);
    }
  }
  *output_w = p->output_w;
  *output_h = p->output_h;
  *rotation = p->rotation;
  *base_render_w = p->default_render_w;
  *base_render_h = p->default_render_h;

  char rot_buf[16];
  if (read_marker_text("berry-device-rotation", rot_buf, sizeof(rot_buf))) {
    int r = atoi(rot_buf);
    if (r == 0 || r == 90 || r == 180 || r == 270)
      *rotation = r;
  }
}

static void scale_render_dims(int* render_w,
                              int* render_h,
                              int base_w,
                              int base_h,
                              float scale) {
  int w = (int)(base_w * scale + 0.5f);
  int h = (int)(base_h * scale + 0.5f);
  if (w < 320)
    w = 320;
  if (h < 320)
    h = 320;
  *render_w = w;
  *render_h = h;
}

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

static void write_marker_text(const char* name, const char* val) {
  char path[512];
  snprintf(path, sizeof(path), "%s%s", kSharedMisc, name);
  FILE* f = fopen(path, "w");
  if (!f)
    return;
  fputs(val, f);
  fclose(f);
}

/* --- Log shipping ---------------------------------------------------------
 * On every boot, POST the tail of berry-kbd.log to a small HTTP endpoint so
 * device logs are collectable without USB/SSH (multiple test devices, remote
 * work). Runs BEFORE the engine starts, so it always carries the PREVIOUS
 * session -- including crash backtraces.
 *
 * Config (shared/misc markers; also exposed in Settings > Developer):
 *   berry-logship-url      host[:port][/path]  plain HTTP; e.g.
 *                          "logs.example.com:8787/ingest?t=secret"
 *   berry-logship.disable  kill switch
 *
 * The upload runs in a forked child with socket timeouts + an alarm() hard
 * stop, so a dead endpoint can never slow or hang boot. Pairs with
 * deploy/logship-server.js (dependency-free node receiver). */
#define BERRY_LOGSHIP_TAIL (192 * 1024)

static void berry_ship_log(int build_num) {
  if (marker_exists("berry-logship.disable"))
    return;
  char url[240];
  if (!read_marker_text("berry-logship-url", url, sizeof(url)))
    return; /* not configured */

  /* Persistent per-device id so multiple phones don't mix logs. */
  char dev_id[64];
  if (!read_marker_text("berry-logship-id", dev_id, sizeof(dev_id))) {
    char model[32] = "bb10";
    read_marker_text("berry-device", model, sizeof(model));
    snprintf(dev_id, sizeof(dev_id), "%s-%05ld", model,
             (long)(time(NULL) % 100000));
    write_marker_text("berry-logship-id", dev_id);
  }

  fprintf(stderr, "BerryShell: logship -> %s (device=%s)\n", url, dev_id);

  pid_t pid = fork();
  if (pid != 0)
    return; /* parent (or failed fork): continue boot immediately */

  /* ---- child: hard cap total time, then best-effort upload ---- */
  alarm(15);

  const char* p = url;
  if (strncmp(p, "http://", 7) == 0)
    p += 7;
  char host[128] = "";
  char portbuf[8] = "80";
  char path[160] = "/ingest";
  const char* slash = strchr(p, '/');
  size_t hostlen = slash ? (size_t)(slash - p) : strlen(p);
  if (hostlen >= sizeof(host))
    _exit(0);
  memcpy(host, p, hostlen);
  host[hostlen] = '\0';
  if (slash && slash[0])
    snprintf(path, sizeof(path), "%s", slash);
  char* colon = strchr(host, ':');
  if (colon) {
    *colon = '\0';
    snprintf(portbuf, sizeof(portbuf), "%s", colon + 1);
  }

  /* Read the log tail. */
  char logpath[512];
  snprintf(logpath, sizeof(logpath), "%sberry-kbd.log", kSharedMisc);
  int lfd = open(logpath, O_RDONLY);
  if (lfd < 0)
    _exit(0);
  struct stat st;
  if (fstat(lfd, &st) != 0 || st.st_size <= 0) {
    close(lfd);
    _exit(0);
  }
  off_t start = st.st_size > BERRY_LOGSHIP_TAIL
                    ? st.st_size - BERRY_LOGSHIP_TAIL
                    : 0;
  lseek(lfd, start, SEEK_SET);
  char* body = (char*)malloc(BERRY_LOGSHIP_TAIL);
  if (!body) {
    close(lfd);
    _exit(0);
  }
  ssize_t body_len = read(lfd, body, BERRY_LOGSHIP_TAIL);
  close(lfd);
  if (body_len <= 0)
    _exit(0);

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* res = NULL;
  if (getaddrinfo(host, portbuf, &hints, &res) != 0 || !res)
    _exit(0);
  int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (s < 0)
    _exit(0);
  struct timeval tv = {5, 0};
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  if (connect(s, res->ai_addr, res->ai_addrlen) != 0)
    _exit(0);

  char sep = strchr(path, '?') ? '&' : '?';
  char hdr[640];
  int hdr_len = snprintf(
      hdr, sizeof(hdr),
      "POST %s%cdevice=%s&build=%d HTTP/1.1\r\n"
      "Host: %s\r\n"
      "Content-Type: text/plain\r\n"
      "Content-Length: %ld\r\n"
      "Connection: close\r\n"
      "\r\n",
      path, sep, dev_id, build_num, host, (long)body_len);
  if (hdr_len > 0 && hdr_len < (int)sizeof(hdr)) {
    write(s, hdr, (size_t)hdr_len);
    ssize_t off = 0;
    while (off < body_len) {
      ssize_t w = write(s, body + off, (size_t)(body_len - off));
      if (w <= 0)
        break;
      off += w;
    }
    /* Drain a little of the response so the server sees a clean close. */
    char resp[128];
    read(s, resp, sizeof(resp));
  }
  close(s);
  _exit(0);
}

/* Load-reduction profiles (Settings > Display / Frame rate, or marker files).
 * Defaults (no markers): 540-tier on 720+ profiles, 45 fps cap, 4 raster threads.
 * Resolution tiers scale the device profile's default render (420/540/720/1440
 * relative to a 720px reference on the short axis). Profiles stack unless
 * noted: berry-x-lite.enable = 420-tier + 12fps + 2 thr;
 * berry-x-perf.enable = 540-tier + 12fps + 2 thr. */
static int has_resolution_tier_marker(void) {
  return marker_exists("berry-x-420.enable") ||
         marker_exists("berry-x-540.enable") ||
         marker_exists("berry-x-720.enable") ||
         marker_exists("berry-x-1440.enable");
}

static void apply_x_load_profile(int* render_w,
                                 int* render_h,
                                 int* max_fps,
                                 int* raster_threads,
                                 int* use_fps_limit_flag,
                                 char* profile_name,
                                 size_t profile_name_len,
                                 int base_render_w,
                                 int base_render_h) {
  float scale = 1.0f;
  strncpy(profile_name, "default", profile_name_len);

  if (marker_exists("berry-x-lite.enable")) {
    scale = 420.0f / 720.0f;
    scale_render_dims(render_w, render_h, base_render_w, base_render_h, scale);
    *max_fps = 12;
    *raster_threads = 2;
    *use_fps_limit_flag = 1;
    strncpy(profile_name, "lite(420-tier+12fps+2thr)", profile_name_len);
    return;
  }
  if (marker_exists("berry-x-perf.enable")) {
    scale = 540.0f / 720.0f;
    scale_render_dims(render_w, render_h, base_render_w, base_render_h, scale);
    *max_fps = 12;
    *raster_threads = 2;
    *use_fps_limit_flag = 1;
    strncpy(profile_name, "perf(540-tier+12fps+2thr)", profile_name_len);
    return;
  }
  if (!has_resolution_tier_marker() && base_render_w >= 720 &&
      base_render_h >= 720) {
    scale = 540.0f / 720.0f;
    strncpy(profile_name, "540 (default)", profile_name_len);
  }
  scale_render_dims(render_w, render_h, base_render_w, base_render_h, scale);

  if (marker_exists("berry-x-420.enable")) {
    scale = 420.0f / 720.0f;
    strncpy(profile_name, "420", profile_name_len);
  }
  if (marker_exists("berry-x-540.enable")) {
    scale = 540.0f / 720.0f;
    strncpy(profile_name, "540", profile_name_len);
  }
  if (marker_exists("berry-x-720.enable")) {
    scale = 1.0f;
    strncpy(profile_name, "720", profile_name_len);
  }
  if (marker_exists("berry-x-1440.enable")) {
    scale = 1440.0f / 720.0f;
    strncpy(profile_name, "1440", profile_name_len);
  }
  scale_render_dims(render_w, render_h, base_render_w, base_render_h, scale);
  if (marker_exists("berry-x-fullfps.enable")) {
    *max_fps = 0;
    *use_fps_limit_flag = 0;
    strncpy(profile_name, "fullfps", profile_name_len);
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
  if (marker_exists("berry-x-slow45.enable")) {
    *max_fps = 45;
    *use_fps_limit_flag = 1;
    strncpy(profile_name, "slow45", profile_name_len);
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
 * intentionally left ENABLED because on-screen qnx_screen rendering needs it.
 * NOTE: Chromium does NOT merge repeated --disable-features switches (last one
 * wins), so ALL feature disables must be combined into the single string built
 * below in main(). */
static const char* kUnstableFeatures =
    "NetworkServiceDedicatedThread,MojoIpcz,"
    "Translate,OptimizationHints,MediaRouter,PreconnectToSearch,"
    "BackForwardCache,AutofillServerCommunication,HeavyAdPrivacyMitigations,"
    "InterestFeedContentSuggestions";

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

  /* GPU/EGL on by default for sharp compositing; FFmpeg software decode handles
   * video (--disable-accelerated-video-decode). Opt out: berry-gpu.disable.
   * Legacy berry-gpu.enable is redundant. QNX_ENABLE_GPU=1 / QNX_DISABLE_GPU=1
   * override markers for SSH testing. */
  int use_gpu = 1;
  if (access("/accounts/1000/shared/misc/berry-gpu.disable", F_OK) == 0)
    use_gpu = 0;
  {
    const char* gpu_env = getenv("QNX_DISABLE_GPU");
    if (gpu_env && gpu_env[0] == '1')
      use_gpu = 0;
  }
  if (access("/accounts/1000/shared/misc/berry-gpu.enable", F_OK) == 0)
    use_gpu = 1;
  {
    const char* gpu_env = getenv("QNX_ENABLE_GPU");
    if (gpu_env && gpu_env[0] == '1')
      use_gpu = 1;
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
                   O_WRONLY | O_CREAT | O_APPEND, 0666);
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
    /* PGO profile collection: write LLVM .profraw to shared/misc/pgo/ and skip
     * the exit watchdog so profiles flush on close/restart. */
    if (access("/accounts/1000/shared/misc/berry-pgo-collect.enable", F_OK) ==
        0) {
      setenv("BERRY_PGO_COLLECT", "1", 1);
      setenv("LLVM_PROFILE_FILE",
             "/accounts/1000/shared/misc/pgo/berry-%p-%m.profraw", 1);
      fprintf(stderr,
              "BerryShell: PGO collect = ON (profiles -> shared/misc/pgo/)\n");
    }
  }

  fprintf(stderr, "BerryShell: Berry Browser build 86\n");

  /* Ship the previous session's log tail (crash backtraces included) to the
   * configured endpoint. Forked; never blocks boot. */
  berry_ship_log(86);

  fprintf(stderr, "BerryShell: app dir = %s\n", dir);
  fprintf(stderr, "BerryShell: work dir (cwd) = %s\n", work);
  fprintf(stderr, "BerryShell: %s\n",
          use_single_process ? "single-process mode" : "multi-process mode");
  fprintf(stderr, "BerryShell: %s (opt-out GPU: berry-gpu.disable in shared/misc)\n",
          use_gpu ? "GPU/EGL mode" : "software mode (--disable-gpu)");

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

  /* Device panel + orientation (Settings > Device, or berry-device marker). */
  int output_w = 1440, output_h = 1440, rotation = 90;
  int base_render_w = 720, base_render_h = 720;
  char device_name[64];
  apply_device_profile(&output_w, &output_h, &rotation, &base_render_w,
                       &base_render_h, device_name, sizeof(device_name));

  /* Default render: device profile, scaled by Display resolution tier. */
  int render_w = base_render_w, render_h = base_render_h;
  int max_fps = 45;
  int raster_threads = 4;
  int use_fps_limit_flag = 1;
  char profile_name[64];
  apply_x_load_profile(&render_w, &render_h, &max_fps, &raster_threads,
                       &use_fps_limit_flag, profile_name, sizeof(profile_name),
                       base_render_w, base_render_h);

  {
    char dim[16];
    char rot[16];
    snprintf(dim, sizeof(dim), "%d", render_w);
    setenv("QNX_SCREEN_WIDTH", dim, 1);
    snprintf(dim, sizeof(dim), "%d", render_h);
    setenv("QNX_SCREEN_HEIGHT", dim, 1);
    snprintf(dim, sizeof(dim), "%d", output_w);
    setenv("QNX_SCREEN_OUTPUT_WIDTH", dim, 1);
    snprintf(dim, sizeof(dim), "%d", output_h);
    setenv("QNX_SCREEN_OUTPUT_HEIGHT", dim, 1);
    snprintf(rot, sizeof(rot), "%d", rotation);
    setenv("QNX_SCREEN_ROTATION", rot, 1);
  }
  /* Faster BPS input drain (keyboard/touch). See qnx_screen_event_source.cc. */
  setenv("QNX_INPUT_POLL_MS", "1", 0);
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
            "BerryShell: device = %s panel=%dx%d rot=%d render=%dx%d "
            "load=%s raster_thr=%d max_fps=%s fps_limit=%s\n",
            device_name, output_w, output_h, rotation, render_w, render_h,
            profile_name, raster_threads,
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
  argv_buf[n++] = (char*)"--disable-notifications";
  argv_buf[n++] = (char*)"--disable-smooth-scrolling";
  argv_buf[n++] = (char*)"--no-first-run";
  argv_buf[n++] = (char*)"--no-default-browser-check";
  argv_buf[n++] = (char*)"--disable-component-update";
  /* YouTube/mobile video: allow <video> autoplay without an extra gesture tap
   * after the user already opened a watch page. */
  argv_buf[n++] = (char*)"--autoplay-policy=no-user-gesture-required";
  /* QNX has no HW video decode; force FFmpeg software path so MSE/DASH and
   * progressive MP4 share the same in-process decoders. */
  argv_buf[n++] = (char*)"--disable-accelerated-video-decode";
  if (marker_exists("berry-video.debug")) {
    setenv("QNX_NAV_DEBUG", "1", 1);
    argv_buf[n++] = (char*)"--enable-logging=stderr";
    if (marker_exists("berry-video.verbose")) {
      argv_buf[n++] = (char*)"--v=1";
    }
    fprintf(stderr,
            "BerryShell: video debug = on (berry-video.debug, nav ChunkDone on)\n");
  } else if (access("/accounts/1000/shared/misc/berry-kbd.debug", F_OK) != 0 &&
             access("/accounts/1000/shared/misc/berry-nav.debug", F_OK) != 0) {
    /* Verbose logging to berry-kbd.log costs measurable CPU/IO on BB10; keep
     * stderr quiet unless a debug marker is present. */
    argv_buf[n++] = (char*)"--disable-logging";
  }
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
  /* QUIC OPT-IN (build 68): QUIC's TLS proof verification fails on QNX
   * (repeated BoringSSL CERTIFICATE_VERIFY_FAILED; the ignore-cert-errors
   * wrapper covers the TCP path but QUIC handshakes still died), which
   * media-timeouts video CDNs advertising HTTP/3 (CBC "Phoenix" player,
   * googlevideo). HTTP/2 is the reliable transport here. Debug via
   * berry-quic.enable. */
  if (marker_exists("berry-quic.enable")) {
    fprintf(stderr, "BerryShell: QUIC = enabled (berry-quic.enable)\n");
  } else {
    argv_buf[n++] = (char*)"--disable-quic";
    fprintf(stderr, "BerryShell: QUIC = disabled (default)\n");
  }
  /* HTTP/2 ON by default (HARDENING tier 1 PASS). Opt out: berry-http1.enable
   * (googlevideo ALPN quirk A/B). */
  if (marker_exists("berry-http1.enable")) {
    argv_buf[n++] = (char*)"--disable-http2";
    fprintf(stderr, "BerryShell: HTTP/2 = disabled (berry-http1.enable)\n");
  } else {
    fprintf(stderr, "BerryShell: HTTP/2 = enabled (default)\n");
  }
  /* Telemetry blackhole: fast-fail non-essential Meta/WhatsApp logging hosts
   * during the load-critical window (DNS + connections + JS). Conservative
   * list — chat UI / CDN / login hosts are untouched. Default ON; opt out:
   * berry-block.disable. Legacy berry-block.enable is redundant. */
  if (!marker_exists("berry-block.disable")) {
    static char block_rules[768];
    snprintf(
        block_rules, sizeof(block_rules),
        "--host-resolver-rules="
        "MAP crashlogs.whatsapp.net 0.0.0.0,"
        "MAP pixel.facebook.com 0.0.0.0,"
        "MAP analytics.facebook.com 0.0.0.0,"
        "MAP metric.facebook.com 0.0.0.0");
    argv_buf[n++] = block_rules;
    fprintf(stderr,
            "BerryShell: telemetry blocklist = on (analytics only, graph OK)\n");
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
      /* A/B: berry-msaa0.enable disables MSAA during GPU raster. Trades a
       * little edge smoothing on complex paths for Adreno 330 fill-rate /
       * memory bandwidth. */
      if (marker_exists("berry-msaa0.enable")) {
        argv_buf[n++] = (char*)"--gpu-rasterization-msaa-sample-count=0";
        fprintf(stderr, "BerryShell: gpu raster MSAA = 0 (berry-msaa0.enable)\n");
      }
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
  /* Single merged --disable-features switch. Chromium keeps only the LAST
   * occurrence of a repeated switch, so the stability list, the privacy-sandbox
   * cuts, and the Alt-Svc A/B must be combined here (they used to be separate
   * switches and silently cancelled each other — only the last one applied). */
  {
    static char disable_features[768];
    snprintf(disable_features, sizeof(disable_features), "--disable-features=%s",
             kUnstableFeatures);
    /* ServiceWorker A/B: ON by default (YouTube/PWA app-shell caching). Opt
     * out: berry-sw.disable. Legacy berry-sw.enable is redundant. */
    if (marker_exists("berry-sw.disable")) {
      strncat(disable_features, ",ServiceWorker",
              sizeof(disable_features) - strlen(disable_features) - 1);
      fprintf(stderr,
              "BerryShell: ServiceWorker = disabled (berry-sw.disable)\n");
    } else {
      fprintf(stderr, "BerryShell: ServiceWorker = ENABLED (default)\n");
    }
    /* Privacy sandbox / identity APIs: FedCM, ads measurement, etc. —
     * background work with no BB10 benefit. Default OFF; opt back in:
     * berry-privacy.enable. */
    if (!marker_exists("berry-privacy.enable")) {
      strncat(disable_features,
              ",FedCm,PrivacySandboxAdsAPIs,SharedStorageAPI,"
              "PrivateAggregationApi",
              sizeof(disable_features) - strlen(disable_features) - 1);
      fprintf(stderr, "BerryShell: privacy sandbox APIs = disabled (default)\n");
    }
    /* Alt-Svc/SVCB A/B: stop DNS HTTPS records from advertising HTTP/3 paths. */
    if (marker_exists("berry-alt-svc.disable")) {
      strncat(disable_features, ",UseDnsHttpsSvcb,UseDnsHttpsSvcbAlpn",
              sizeof(disable_features) - strlen(disable_features) - 1);
      fprintf(stderr,
              "BerryShell: Alt-Svc/SVCB = disabled (berry-alt-svc.disable)\n");
    }
    argv_buf[n++] = disable_features;
  }
  /* Low-end device mode ON by default (smaller V8 heap + leaner tile caches;
   * big win on heavy SPAs like WhatsApp). Opt out: berry-lowend.disable.
   * Legacy berry-lowend.enable is redundant. */
  if (!marker_exists("berry-lowend.disable")) {
    argv_buf[n++] = (char*)"--enable-low-end-device-mode";
    fprintf(stderr, "BerryShell: low-end device mode = ON (default)\n");
  }
  /* Larger HTTP cache for repeat visits (YouTube thumbs, Maps tiles, SW shells).
   * Opt out: berry-disk-cache.disable */
  if (!marker_exists("berry-disk-cache.disable")) {
    argv_buf[n++] = (char*)"--disk-cache-size=268435456";
    argv_buf[n++] = (char*)"--media-cache-size=268435456";
    fprintf(stderr, "BerryShell: disk+media cache = 256MB (default)\n");
  }
  /* Present as Android Chrome unless a desktop-gated host forced desktop above.
   * content_shell's GetUserAgent()/GetUserAgentMetadata() and OverrideWebkitPrefs
   * key off this switch to emit a mobile UA, Sec-CH-UA-Mobile: ?1, and mobile
   * viewport layout. */
  if (use_mobile_ua)
    argv_buf[n++] = (char*)"--use-mobile-user-agent";
  if (use_custom_ua)
    argv_buf[n++] = ua_arg;
  /* V8 flags. berry-jsflags (text marker) overrides everything for A/B tests.
   * Defaults otherwise:
   *   --max-old-space-size=256 — low-end device mode caps the JS heap at
   *     ~128MB, which OOM-crashes Messenger/Facebook ("Ineffective
   *     mark-compacts near heap limit", seen Aug 27). The Passport has 3GB;
   *     give JS 256MB while keeping the rest of low-end mode's savings.
   *     Opt out: berry-heap.default marker.
   *   WASM liftoff-only stays OPT-IN (berry-wasm-liftoff.enable): default
   *     leaves V8 tier-up on so Messenger/Facebook login crypto works. */
  {
    static char jsflags_buf[1024];
    static char jsflags_arg[1104];
    if (read_marker_text("berry-jsflags", jsflags_buf, sizeof(jsflags_buf))) {
      snprintf(jsflags_arg, sizeof(jsflags_arg), "--js-flags=%s", jsflags_buf);
      argv_buf[n++] = jsflags_arg;
      fprintf(stderr, "BerryShell: js-flags = %s (berry-jsflags)\n", jsflags_buf);
    } else {
      const int big_heap = !marker_exists("berry-heap.default");
      const int liftoff = marker_exists("berry-wasm-liftoff.enable");
      if (big_heap || liftoff) {
        snprintf(jsflags_arg, sizeof(jsflags_arg), "--js-flags=%s%s%s",
                 big_heap ? "--max-old-space-size=256" : "",
                 (big_heap && liftoff) ? " " : "",
                 liftoff ? "--no-wasm-tier-up --wasm-lazy-validation" : "");
        argv_buf[n++] = jsflags_arg;
      }
      if (big_heap)
        fprintf(stderr, "BerryShell: js heap = 256MB old-space (default)\n");
      if (liftoff)
        fprintf(stderr,
                "BerryShell: wasm = liftoff-only (berry-wasm-liftoff)\n");
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
    /* Low-end defaults: skip ping-on-click and other tiny background fetches. */
    if (!marker_exists("berry-lowend.disable"))
      strncat(settings, "hyperlinkAuditingEnabled=false,"
                          "spellcheckEnabledByDefault=false,",
              sizeof(settings) - strlen(settings) - 1);
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
