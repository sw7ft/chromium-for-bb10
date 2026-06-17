// Copyright 2026 SW7FT. All rights reserved.

#include "content/shell/browser/qnx_berry_daemon_host.h"

#include "build/build_config.h"

#if BUILDFLAG(IS_QNX)

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include "base/functional/bind.h"
#include "base/qnx_berry_daemon.h"
#include "base/strings/string_util.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/shell/browser/shell.h"
#include "url/gurl.h"
#include "url/url_constants.h"

namespace content {

namespace {

const char kBerryCmdPortEnv[] = "QNX_BERRY_CMD_PORT";
const int kDefaultCmdPort = 8767;

int g_listen_fd = -1;
bool g_busy = false;
bool g_shutdown = false;

base::WaitableEvent g_render_complete(
    base::WaitableEvent::ResetPolicy::AUTOMATIC,
    base::WaitableEvent::InitialState::NOT_SIGNALED);

int CommandPort() {
  const char* port_str = getenv(kBerryCmdPortEnv);
  if (!port_str || !port_str[0])
    return kDefaultCmdPort;
  int port = atoi(port_str);
  return (port > 0 && port < 65536) ? port : kDefaultCmdPort;
}

void SchedulePump();
void PumpCommands();

bool ReadLineFromSocket(int fd, std::string* out) {
  out->clear();
  char ch = 0;
  while (true) {
    ssize_t n = read(fd, &ch, 1);
    if (n <= 0)
      return false;
    if (ch == '\n')
      return true;
    if (out->size() < 4096)
      out->push_back(ch);
  }
}

void OnRenderTimeout() {
  if (!g_busy)
    return;
  if (!g_render_complete.IsSignaled())
    base::QnxBerryDaemonEmitError("render timeout");
  g_busy = false;
  SchedulePump();
}

void CheckRenderComplete() {
  if (!g_busy)
    return;
  if (g_render_complete.IsSignaled()) {
    g_busy = false;
    SchedulePump();
    return;
  }
  GetUIThreadTaskRunner({})->PostDelayedTask(
      FROM_HERE, base::BindOnce(&CheckRenderComplete), base::Milliseconds(500));
}

void LoadUrlOnUiThread(const GURL& url) {
  fprintf(stderr, "QNX: berry-daemon LoadURL %s\n", url.spec().c_str());
  fflush(stderr);
  if (Shell::windows().empty()) {
    base::QnxBerryDaemonEmitError("no shell window");
    g_busy = false;
    SchedulePump();
    return;
  }
  Shell* shell = Shell::windows()[0];
  shell->ResetForBerryDaemonLoad();
  shell->LoadURL(url);
}

void HandleCommand(const std::string& line) {
  if (line == "QUIT") {
    g_shutdown = true;
    Shell::Shutdown();
    return;
  }

  if (g_busy) {
    base::QnxBerryDaemonEmitError("busy");
    SchedulePump();
    return;
  }

  if (!base::StartsWith(line, "LOAD ", base::CompareCase::SENSITIVE)) {
    base::QnxBerryDaemonEmitError("expected LOAD <url>");
    SchedulePump();
    return;
  }

  std::string url_str = line.substr(5);
  GURL url(url_str);
  if (!url.is_valid() ||
      (!url.SchemeIsHTTPOrHTTPS() && !url.SchemeIs(url::kDataScheme))) {
    base::QnxBerryDaemonEmitError("invalid url");
    SchedulePump();
    return;
  }

  g_render_complete.Reset();
  g_busy = true;
  LoadUrlOnUiThread(url);
  GetUIThreadTaskRunner({})->PostDelayedTask(
      FROM_HERE, base::BindOnce(&OnRenderTimeout), base::Seconds(120));
  CheckRenderComplete();
}

void PumpCommands() {
  if (g_shutdown || g_listen_fd < 0)
    return;
  if (g_busy) {
    SchedulePump();
    return;
  }

  int client_fd = accept(g_listen_fd, nullptr, nullptr);
  if (client_fd < 0) {
    SchedulePump();
    return;
  }

  int flags = fcntl(client_fd, F_GETFL, 0);
  if (flags >= 0)
    fcntl(client_fd, F_SETFL, flags & ~O_NONBLOCK);

  std::string line;
  if (ReadLineFromSocket(client_fd, &line)) {
    base::TrimWhitespaceASCII(line, base::TRIM_ALL, &line);
    if (!line.empty()) {
      fprintf(stderr, "QNX: berry-daemon cmd: %s\n", line.c_str());
      fflush(stderr);
      HandleCommand(line);
    }
  }
  close(client_fd);

  if (!g_shutdown && !g_busy)
    SchedulePump();
}

void SchedulePump() {
  if (g_shutdown)
    return;
  GetUIThreadTaskRunner({})->PostDelayedTask(
      FROM_HERE, base::BindOnce(&PumpCommands), base::Milliseconds(100));
}

bool InitCommandSocket() {
  const int port = CommandPort();
  g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (g_listen_fd < 0)
    return false;

  int reuse = 1;
  setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  int flags = fcntl(g_listen_fd, F_GETFL, 0);
  if (flags >= 0)
    fcntl(g_listen_fd, F_SETFL, flags | O_NONBLOCK);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(static_cast<uint16_t>(port));

  if (bind(g_listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    close(g_listen_fd);
    g_listen_fd = -1;
    return false;
  }

  if (listen(g_listen_fd, 4) < 0) {
    close(g_listen_fd);
    g_listen_fd = -1;
    return false;
  }

  fprintf(stderr, "QNX: berry-daemon listening on 127.0.0.1:%d\n", port);
  fflush(stderr);
  return true;
}

}  // namespace

void StartQnxBerryDaemonHost() {
  if (!base::QnxBerryDaemonEnabled()) {
    fprintf(stderr, "QNX: berry-daemon not enabled\n");
    fflush(stderr);
    return;
  }
  base::QnxBerryDaemonSetRenderCompleteEvent(&g_render_complete);
  if (!InitCommandSocket()) {
    fprintf(stderr, "QNX: berry-daemon socket init failed\n");
    fflush(stderr);
    return;
  }
  GetUIThreadTaskRunner({})->PostTask(FROM_HERE, base::BindOnce(&SchedulePump));
}

}  // namespace content

#else

namespace content {
void StartQnxBerryDaemonHost() {}
}  // namespace content

#endif  // BUILDFLAG(IS_QNX)
