// Copyright 2026 SW7FT. All rights reserved.

#include "base/qnx_berry_daemon.h"

#if BUILDFLAG(IS_QNX)

#include "base/qnx_hard_watchdog.h"

#include <unistd.h>

#include <cstdlib>
#include <cstdio>
#include <string>

#include "base/command_line.h"
#include "base/functional/callback.h"
#include "base/containers/span.h"
#include "base/strings/string_piece.h"
#include "base/synchronization/lock.h"
#include "base/synchronization/waitable_event.h"

namespace base {

namespace {

const char kBerryDaemonSwitch[] = "berry-daemon";
const char kBerryDaemonEnv[] = "QNX_BERRY_DAEMON";
const char kBerryDaemonMarkerEnv[] = "QNX_BERRY_DAEMON_MARKER";

bool BerryDaemonMarkerPresent() {
  const char* marker = getenv(kBerryDaemonMarkerEnv);
  if (marker && marker[0] && access(marker, F_OK) == 0)
    return true;
  const char* exe = getenv("CHROME_EXE_PATH");
  if (exe && exe[0]) {
    std::string path(exe);
    size_t pos = path.find_last_of('/');
    if (pos != std::string::npos) {
      path.replace(pos + 1, std::string::npos, "berry-daemon.mode");
      if (access(path.c_str(), F_OK) == 0)
        return true;
    }
  }
  return false;
}

Lock g_completion_lock;
WaitableEvent* g_render_complete = nullptr;
bool g_berry_daemon_forced = false;

void WriteAll(const char* data, size_t len) {
  while (len > 0) {
    ssize_t n = write(1, data, len);
    if (n <= 0)
      return;
    data += n;
    len -= static_cast<size_t>(n);
  }
}

void WriteLine(base::StringPiece line) {
  WriteAll(line.data(), line.size());
  WriteAll("\n", 1);
}

void WriteLineString(const std::string& line) {
  WriteLine(line);
}

}  // namespace

bool QnxBerryDaemonEnabled() {
  if (g_berry_daemon_forced)
    return true;
  if (CommandLine::ForCurrentProcess()->HasSwitch(kBerryDaemonSwitch))
    return true;
  // Marker beside content_shell (child re-execs may drop switches/env).
  if (BerryDaemonMarkerPresent())
    return true;
  const char* env = getenv(kBerryDaemonEnv);
  return env && env[0] == '1';
}

void QnxBerryDaemonForceEnable() {
  g_berry_daemon_forced = true;
}

bool QnxBerryDaemonForced() {
  return g_berry_daemon_forced;
}

void QnxBerryDaemonSetRenderCompleteEvent(WaitableEvent* event) {
  AutoLock lock(g_completion_lock);
  g_render_complete = event;
}

void QnxBerryDaemonEmitHtml(base::span<const char> html) {
  CancelQnxBootWatchdog();
  CancelQnxHardWatchdog();
  if (QnxBerryDaemonEnabled()) {
    WriteLineString("@BERRY DOM " + std::to_string(html.size()));
    if (!html.empty())
      WriteAll(html.data(), html.size());
    WriteLine("@BERRY END");
    AutoLock lock(g_completion_lock);
    if (g_render_complete)
      g_render_complete->Signal();
    fflush(stdout);
    return;
  }

  if (!html.empty())
    WriteAll(html.data(), html.size());
  // Never _exit in persistent daemon mode (marker/env); one-shot uses raw exit.
  if (!BerryDaemonMarkerPresent()) {
    const char* env = getenv(kBerryDaemonEnv);
    if (!env || env[0] != '1')
      _exit(0);
  }
}

void QnxBerryDaemonEmitError(const char* message) {
  if (!QnxBerryDaemonEnabled()) {
    _exit(1);
    return;
  }
  WriteLineString(std::string("@BERRY ERR ") + message);
  WriteLine("@BERRY END");
  AutoLock lock(g_completion_lock);
  if (g_render_complete)
    g_render_complete->Signal();
}

}  // namespace base

#endif  // BUILDFLAG(IS_QNX)
