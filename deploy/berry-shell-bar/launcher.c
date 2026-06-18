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

static const char* kDefaultUrl = "https://www.google.com/";

/* Display tuning — easy to change without rebuilding content_shell.
 * kRotation: Navigator composites the app window rotated; "90" is the first
 *   guess to undo the observed 90-deg-clockwise tilt. Try 0/90/180/270.
 * kScaleFactor: 1440x1440 is very high DPI (~453ppi); scale up so text/UI are
 *   legible. Passed to content_shell as --force-device-scale-factor. */
static const char* kRotation = "90";
static const char* kScaleFactor = "--force-device-scale-factor=2";

/* Stability: these networking/IPC features are documented (HARDENING.md) as
 * unstable on QNX — they deadlock or race the resource loader (the ~20-60s
 * crash on heavy sites like google.com was RawResource::NotifyFinished hitting
 * an invalid state via the dedicated network thread + sync cookie IPC). Viz is
 * intentionally left ENABLED because on-screen qnx_screen rendering needs it. */
static const char* kDisableFeatures =
    "--disable-features=ServiceWorker,NetworkServiceDedicatedThread,MojoIpcz";

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
  chdir(dir);
  fprintf(stderr, "BerryShell: app dir = %s\n", dir);

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

  char shell[2100];
  snprintf(shell, sizeof(shell), "%s/content_shell", dir);

  const char* url = (argc > 1 && argv[1] && argv[1][0]) ? argv[1] : kDefaultUrl;

  char* const new_argv[] = {
      shell,
      (char*)"--no-sandbox",
      (char*)"--no-zygote",
      (char*)"--single-process",
      (char*)"--disable-gpu",
      (char*)"--disable-gpu-compositing",
      (char*)"--ozone-platform=qnx_screen",
      (char*)kScaleFactor,
      (char*)kDisableFeatures,
      (char*)url,
      NULL,
  };

  execv(shell, new_argv);

  /* Only reached if exec fails. */
  perror("BerryShell: execv content_shell failed");
  return 1;
}
