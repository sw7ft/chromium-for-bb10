// Copyright 2026 SW7FT. All rights reserved.

#include "base/qnx_hard_watchdog.h"

#if BUILDFLAG(IS_QNX)

#include <atomic>
#include <cstdio>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>

namespace base {

namespace {

constexpr int kSleepSliceMs = 500;

// One independent watchdog per phase. Fields are written before the detached
// thread is created (happens-before via pthread_create) and only |cancelled| is
// mutated afterward, so a plain atomic flag is sufficient.
struct WatchdogState {
  std::atomic<bool> cancelled{false};
  int total_ms = 0;
  const char* label = "HardWatchdog";
  bool dump_threads = false;
};

WatchdogState g_commit_watchdog;
WatchdogState g_boot_watchdog;

// Before exiting on a deadline, ask every thread to dump its stack via the
// SIGUSR2 sampler (registered in base/debug/stack_trace_posix.cc). On QNX a
// pthread_t is the integer tid, so brute-force the low tid range; nonexistent
// tids return ESRCH and are skipped. A short pause between signals lets each
// handler emit its (atomic) line without interleaving.
void DumpAllThreadStacks() {
  const char hdr[] = "QNX:Watchdog dumping all thread stacks (SIGUSR2)\n";
  write(2, hdr, sizeof(hdr) - 1);
  pthread_t self = pthread_self();
  for (int tid = 1; tid <= 64; ++tid) {
    pthread_t t = static_cast<pthread_t>(tid);
    if (t == self)
      continue;
    if (pthread_kill(t, SIGUSR2) != 0)
      continue;
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 30 * 1000 * 1000L;  // 30ms
    nanosleep(&ts, nullptr);
  }
  const char done[] = "QNX:Watchdog stack dump done\n";
  write(2, done, sizeof(done) - 1);
}

void* WatchdogThread(void* arg) {
  WatchdogState* wd = static_cast<WatchdogState*>(arg);
  int elapsed_ms = 0;
  while (elapsed_ms < wd->total_ms) {
    if (wd->cancelled.load(std::memory_order_acquire))
      return nullptr;
    const int wait_ms = std::min(kSleepSliceMs, wd->total_ms - elapsed_ms);
    struct timespec ts;
    ts.tv_sec = wait_ms / 1000;
    ts.tv_nsec = static_cast<long>((wait_ms % 1000) * 1000000L);
    nanosleep(&ts, nullptr);
    elapsed_ms += wait_ms;
  }
  if (wd->cancelled.load(std::memory_order_acquire))
    return nullptr;
  char msg[96];
  int n = snprintf(msg, sizeof(msg), "QNX:%s deadline reached, _exit\n",
                   wd->label);
  if (n > 0)
    write(2, msg, static_cast<size_t>(n));
  if (wd->dump_threads)
    DumpAllThreadStacks();
  _exit(0);
  return nullptr;
}

void StartWatchdog(WatchdogState* wd, int total_ms, const char* label,
                   bool dump_threads) {
  wd->total_ms = total_ms;
  wd->label = label;
  wd->dump_threads = dump_threads;
  wd->cancelled.store(false, std::memory_order_release);
  pthread_t tid;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_create(&tid, &attr, WatchdogThread, wd);
  pthread_attr_destroy(&attr);
}

}  // namespace

void StartQnxHardWatchdog(int total_ms) {
  StartWatchdog(&g_commit_watchdog, total_ms, "HardWatchdog",
                /*dump_threads=*/false);
}

void CancelQnxHardWatchdog() {
  g_commit_watchdog.cancelled.store(true, std::memory_order_release);
}

void StartQnxBootWatchdog(int total_ms) {
  StartWatchdog(&g_boot_watchdog, total_ms, "BootWatchdog",
                /*dump_threads=*/true);
}

void CancelQnxBootWatchdog() {
  g_boot_watchdog.cancelled.store(true, std::memory_order_release);
}

void QnxDumpAllThreadStacks() {
  DumpAllThreadStacks();
}

}  // namespace base

#endif  // BUILDFLAG(IS_QNX)
