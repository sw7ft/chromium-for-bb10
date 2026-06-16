// Copyright 2026 SW7FT. All rights reserved.

#include "base/qnx_hard_watchdog.h"

#if BUILDFLAG(IS_QNX)

#include <atomic>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>

namespace base {

namespace {

constexpr int kSleepSliceMs = 500;

std::atomic<bool> g_watchdog_cancelled{false};

void* QnxHardWatchdogThread(void* arg) {
  const int total_ms = static_cast<int>(reinterpret_cast<intptr_t>(arg));
  int elapsed_ms = 0;
  while (elapsed_ms < total_ms) {
    if (g_watchdog_cancelled.load(std::memory_order_acquire))
      return nullptr;
    const int wait_ms =
        std::min(kSleepSliceMs, total_ms - elapsed_ms);
    struct timespec ts;
    ts.tv_sec = wait_ms / 1000;
    ts.tv_nsec = static_cast<long>((wait_ms % 1000) * 1000000L);
    nanosleep(&ts, nullptr);
    elapsed_ms += wait_ms;
  }
  if (g_watchdog_cancelled.load(std::memory_order_acquire))
    return nullptr;
  const char msg[] = "QNX:HardWatchdog deadline reached, _exit\n";
  write(2, msg, sizeof(msg) - 1);
  _exit(0);
  return nullptr;
}

}  // namespace

void StartQnxHardWatchdog(int total_ms) {
  g_watchdog_cancelled.store(false, std::memory_order_release);
  pthread_t tid;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_create(&tid, &attr, QnxHardWatchdogThread,
                 reinterpret_cast<void*>(static_cast<intptr_t>(total_ms)));
  pthread_attr_destroy(&attr);
}

void CancelQnxHardWatchdog() {
  g_watchdog_cancelled.store(true, std::memory_order_release);
}

}  // namespace base

#endif  // BUILDFLAG(IS_QNX)
