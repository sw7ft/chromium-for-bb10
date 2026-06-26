// Copyright 2026 SW7FT. All rights reserved.
// QNX hard deadline watchdog for --timeout (independent of message loop).

#ifndef BASE_QNX_HARD_WATCHDOG_H_
#define BASE_QNX_HARD_WATCHDOG_H_

#include "build/build_config.h"

namespace base {

#if BUILDFLAG(IS_QNX)

// Commit-phase watchdog: armed at navigation commit for the --timeout deadline.
// Starts a detached thread that _exit(0) after |total_ms| unless cancelled.
void StartQnxHardWatchdog(int total_ms);

// Cancel the commit-phase watchdog (e.g. before parse-time DOM dump exits).
void CancelQnxHardWatchdog();

// Boot-phase watchdog: armed at process start for one-shot --dump-dom runs so an
// init-time deadlock (e.g. Viz host) that hangs before any navigation commits
// still self-terminates. Cancelled by Shell once a real page commits.
void StartQnxBootWatchdog(int total_ms);

// Cancel the boot-phase watchdog (e.g. on first real navigation commit).
void CancelQnxBootWatchdog();

// Exit-phase watchdog: armed when the Navigator asks the app to close
// (NAVIGATOR_EXIT) right before the graceful Shell::Shutdown(). Guarantees the
// process terminates even if teardown hangs on a busy in-process renderer or a
// stuck present/RunUntilIdle loop, so the app window closing always tears down
// content_shell instead of leaving an orphan. Independent of the message loop.
void StartQnxExitWatchdog(int total_ms);

// On-demand: pthread_kill(SIGUSR2) every thread so each emits its own
// (CFI-unwound, symbolized) backtrace via the sampler in stack_trace_posix.cc.
// Async-signal-safe (pthread_kill/nanosleep/write only), so it is safe to call
// directly from a signal handler to dump a hung process's full thread state.
void QnxDumpAllThreadStacks();

#else

inline void StartQnxHardWatchdog(int total_ms) {}
inline void CancelQnxHardWatchdog() {}
inline void StartQnxBootWatchdog(int total_ms) {}
inline void CancelQnxBootWatchdog() {}
inline void StartQnxExitWatchdog(int total_ms) {}

#endif  // BUILDFLAG(IS_QNX)

}  // namespace base

#endif  // BASE_QNX_HARD_WATCHDOG_H_
