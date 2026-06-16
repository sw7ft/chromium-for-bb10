// Copyright 2026 SW7FT. All rights reserved.
// QNX hard deadline watchdog for --timeout (independent of message loop).

#ifndef BASE_QNX_HARD_WATCHDOG_H_
#define BASE_QNX_HARD_WATCHDOG_H_

#include "build/build_config.h"

namespace base {

#if BUILDFLAG(IS_QNX)

// Start a detached thread that _exit(0) after |total_ms| unless cancelled.
void StartQnxHardWatchdog(int total_ms);

// Cancel an armed watchdog (e.g. before parse-time DOM dump exits cleanly).
void CancelQnxHardWatchdog();

#else

inline void StartQnxHardWatchdog(int total_ms) {}
inline void CancelQnxHardWatchdog() {}

#endif  // BUILDFLAG(IS_QNX)

}  // namespace base

#endif  // BASE_QNX_HARD_WATCHDOG_H_
