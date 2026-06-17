// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "build/build_config.h"
#include "content/public/app/content_main.h"
#include "content/shell/app/shell_main_delegate.h"

#if BUILDFLAG(IS_WIN)
#include "base/win/dark_mode_support.h"
#include "base/win/win_util.h"
#include "content/public/app/sandbox_helper_win.h"
#include "sandbox/win/src/sandbox_types.h"
#endif

#if BUILDFLAG(IS_IOS)
#include "base/at_exit.h"                                 // nogncheck
#include "base/command_line.h"                            // nogncheck
#include "content/public/common/content_switches.h"       // nogncheck
#include "content/shell/app/ios/shell_application_ios.h"
#include "content/shell/app/ios/web_tests_support_ios.h"
#include "content/shell/common/shell_switches.h"
#endif

#if BUILDFLAG(IS_WIN)

#if !defined(WIN_CONSOLE_APP)
int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, wchar_t*, int) {
#else
int main() {
  HINSTANCE instance = GetModuleHandle(NULL);
#endif
  // Load and pin user32.dll and uxtheme.dll to avoid having to load them once
  // tests start while on the main thread loop where blocking calls are
  // disallowed. This will also ensure the Windows dark mode support is enabled
  // for the app if available.
  base::win::PinUser32();
  base::win::AllowDarkModeForApp(true);
  sandbox::SandboxInterfaceInfo sandbox_info = {nullptr};
  content::InitializeSandboxInfo(&sandbox_info);
  content::ShellMainDelegate delegate;

  content::ContentMainParams params(&delegate);
  params.instance = instance;
  params.sandbox_info = &sandbox_info;
  return content::ContentMain(std::move(params));
}

#elif BUILDFLAG(IS_IOS)

int main(int argc, const char** argv) {
  // Create this here since it's needed to start the crash handler.
  base::AtExitManager at_exit;

  // Check if this is the browser process or a subprocess. Only the browser
  // browser should run UIApplicationMain.
  base::CommandLine::Init(argc, argv);
  auto type = base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
      switches::kProcessType);

  // The browser process has no --process-type argument.
  if (type.empty()) {
    if (switches::IsRunWebTestsSwitchPresent()) {
      // We create a simple UIApplication to run the web tests.
      return RunWebTestsFromIOSApp(argc, argv);
    } else {
      // We will create the ContentMainRunner once the UIApplication is ready.
      return RunShellApplication(argc, argv);
    }
  } else {
    content::ShellMainDelegate delegate;
    content::ContentMainParams params(&delegate);
    params.argc = argc;
    params.argv = argv;
    return content::ContentMain(std::move(params));
  }
}

#else

#if BUILDFLAG(IS_QNX)
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>
#include <signal.h>
#include <ucontext.h>
#include "base/qnx_trace.h"
#include "base/qnx_berry_daemon.h"
#include "base/qnx_hard_watchdog.h"
#include "gpu/qnx/gpu_qnx.h"

static void qnx_hex(unsigned long v, char* buf, int len) {
  for (int i = len - 1; i >= 0; --i) {
    int d = v & 0xf;
    buf[i] = d < 10 ? '0' + d : 'a' + d - 10;
    v >>= 4;
  }
}

// Runtime trace gate: sending SIGUSR1 flips on g_qnx_trace_enabled so we can
// boot fully untraced (no stderr backpressure / timing distortion) and then
// enable QNX_TRACE_* output just before issuing a decisive probe action.
// Setting a plain bool is async-signal-safe enough here (single writer, all
// reader threads poll it frequently).
static void qnx_usr1_trace_handler(int /*sig*/) {
  g_qnx_trace_enabled = true;
  static const char msg[] = "QNX:TRACE: enabled via SIGUSR1; dumping threads\n";
  write(2, msg, sizeof(msg) - 1);
  // Dump every thread's real (CFI-unwound) backtrace. pthread_kill/nanosleep/
  // write are async-signal-safe, so this is safe from the handler and lets us
  // capture a hung process's full thread state on demand.
  base::QnxDumpAllThreadStacks();
}

static void qnx_segv_handler(int sig, siginfo_t* info, void* ctx) {
  char line[128];
  ucontext_t* uc = (ucontext_t*)ctx;
  // Print fault address
  write(2, "QNX SEGV: addr=0x", 17);
  qnx_hex((unsigned long)info->si_addr, line, 8);
  line[8] = '\n'; write(2, line, 9);
  // Print PC (R15)
  write(2, "QNX SEGV: pc=0x", 15);
  qnx_hex((unsigned long)uc->uc_mcontext.cpu.gpr[15], line, 8);
  line[8] = '\n'; write(2, line, 9);
  // Print LR (R14) - this is the return address / caller of strlen
  write(2, "QNX SEGV: lr=0x", 15);
  qnx_hex((unsigned long)uc->uc_mcontext.cpu.gpr[14], line, 8);
  line[8] = '\n'; write(2, line, 9);
  // Print a few more registers for context: R0-R3 (function args)
  for (int i = 0; i < 4; i++) {
    char prefix[] = "QNX SEGV: r0=0x";
    prefix[13] = '0' + i;
    write(2, prefix, 15);
    qnx_hex((unsigned long)uc->uc_mcontext.cpu.gpr[i], line, 8);
    line[8] = '\n'; write(2, line, 9);
  }
  // Print SP (R13)
  write(2, "QNX SEGV: sp=0x", 15);
  qnx_hex((unsigned long)uc->uc_mcontext.cpu.gpr[13], line, 8);
  line[8] = '\n'; write(2, line, 9);
  // Print a few stack words (potential return addresses)
  unsigned long* sp = (unsigned long*)uc->uc_mcontext.cpu.gpr[13];
  write(2, "QNX SEGV: stack:\n", 17);
  for (int i = 0; i < 16; i++) {
    qnx_hex((unsigned long)sp[i], line, 8);
    line[8] = ' '; line[9] = '\n';
    write(2, line, 10);
  }
  // Re-raise to get QNX's default handler output
  signal(sig, SIG_DFL);
  raise(sig);
}
#endif

int main(int argc, const char** argv) {
#if BUILDFLAG(IS_QNX)
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--qnx-trace") == 0)
      g_qnx_trace_enabled = true;
    if (strcmp(argv[i], "--berry-daemon") == 0)
      base::QnxBerryDaemonForceEnable();
  }
  {
    const char* trace_env = getenv("QNX_TRACE");
    if (trace_env && trace_env[0] == '1')
      g_qnx_trace_enabled = true;
    const char* berry_env = getenv("QNX_BERRY_DAEMON");
    if (berry_env && berry_env[0] == '1')
      base::QnxBerryDaemonForceEnable();
  }
  QNX_TRACE_MSG("QNX: main() entered\n");
  {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = qnx_segv_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    struct sigaction usr1;
    memset(&usr1, 0, sizeof(usr1));
    usr1.sa_handler = qnx_usr1_trace_handler;
    sigemptyset(&usr1.sa_mask);
    sigaction(SIGUSR1, &usr1, nullptr);
  }
  // Set CHROME_EXE_PATH from argv[0] so ICU loader can find data files
  if (argc > 0 && argv[0]) {
    setenv("CHROME_EXE_PATH", argv[0], 1);
    char marker_path[1024];
    marker_path[0] = '\0';
    const char* last_slash = strrchr(argv[0], '/');
    if (last_slash) {
      size_t dir_len = static_cast<size_t>(last_slash - argv[0] + 1);
      if (dir_len + 20 < sizeof(marker_path)) {
        memcpy(marker_path, argv[0], dir_len);
        memcpy(marker_path + dir_len, "berry-daemon.mode", 18);
        marker_path[dir_len + 18] = '\0';
        if (access(marker_path, F_OK) == 0)
          base::QnxBerryDaemonForceEnable();
      }
    }
  }
  // Pre-commit safety watchdog. During single-process one-shot --dump-dom
  // bring-up, an init deadlock (e.g. Viz host) can hang before any navigation
  // commits, so the post-commit --timeout watchdog never arms and the device
  // wedges. Arm a generous boot-deadline watchdog now; Shell cancels it once a
  // real page commits. Skip child processes (--type=*) and daemon mode.
  // Override with QNX_BOOT_WATCHDOG_MS (0 disables).
  {
    bool is_child = false;
    bool has_dump_dom = false;
    for (int i = 1; i < argc; ++i) {
      if (strncmp(argv[i], "--type=", 7) == 0)
        is_child = true;
      if (strcmp(argv[i], "--dump-dom") == 0)
        has_dump_dom = true;
    }
    int boot_ms = 90000;
    const char* boot_env = getenv("QNX_BOOT_WATCHDOG_MS");
    if (boot_env && boot_env[0])
      boot_ms = atoi(boot_env);
    // Use QnxBerryDaemonForced() (not QnxBerryDaemonEnabled()) here: the global
    // CommandLine is not initialized until ContentMain, and Enabled() queries
    // it. All daemon signals are force-enabled above before this point.
    if (!is_child && has_dump_dom && boot_ms > 0 &&
        !base::QnxBerryDaemonForced()) {
      QNX_TRACE_FMT("QNX:BootWatchdog armed %dms\n", boot_ms);
      base::StartQnxBootWatchdog(boot_ms);
    }
  }
  // Determine a writable base directory from exe path or cwd.
  // Force-override TMPDIR/HOME because device defaults (/tmp) may not be
  // writable by the devuser account. Shared memory creation uses TMPDIR.
  {
    char cwd_buf[1024];
    const char* writable_dir = getcwd(cwd_buf, sizeof(cwd_buf));
    if (!writable_dir) writable_dir = ".";
    // Create a dedicated tmp subdirectory so temp files don't collide with
    // deploy files, and QNX mkstemp works in a clean directory.
    char tmp_buf[1100];
    snprintf(tmp_buf, sizeof(tmp_buf), "%s/tmp", writable_dir);
    mkdir(tmp_buf, 0777);
    setenv("HOME", writable_dir, 0);
    setenv("TMPDIR", tmp_buf, 1);
    setenv("XDG_CONFIG_HOME", writable_dir, 1);
    setenv("XDG_CACHE_HOME", writable_dir, 1);
    setenv("XDG_DATA_HOME", writable_dir, 1);
    setenv("XDG_RUNTIME_DIR", tmp_buf, 1);
    fprintf(stderr, "QNX: writable_dir=%s TMPDIR=%s\n",
            writable_dir, getenv("TMPDIR"));
  }
  if (!getenv("USER")) setenv("USER", "user", 1);
  if (!getenv("LOGNAME")) setenv("LOGNAME", "user", 1);
  if (!getenv("SHELL")) setenv("SHELL", "/bin/sh", 1);
  if (!getenv("LANG")) setenv("LANG", "C", 1);
  if (getenv("QNX_GPU_PROBE")) {
    if (gpu::qnx::InitializeGpuLibraries())
      gpu::qnx::PrintGpuInfo();
  }
  QNX_TRACE_MSG("QNX: env vars set, creating delegate\n");
#endif
  content::ShellMainDelegate delegate;
  content::ContentMainParams params(&delegate);
  params.argc = argc;
  params.argv = argv;
#if BUILDFLAG(IS_QNX)
  QNX_TRACE_MSG("QNX: calling ContentMain\n");
#endif
  return content::ContentMain(std::move(params));
}

#endif  // BUILDFLAG(IS_WIN)
