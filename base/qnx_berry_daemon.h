// Copyright 2026 SW7FT. All rights reserved.
// QNX berry-daemon protocol helpers (shared by content_shell and Blink).

#ifndef BASE_QNX_BERRY_DAEMON_H_
#define BASE_QNX_BERRY_DAEMON_H_

#include <stddef.h>

#include "base/containers/span.h"
#include "build/build_config.h"

namespace base {

#if BUILDFLAG(IS_QNX)

class WaitableEvent;

bool QnxBerryDaemonEnabled();

// Called from main() before ContentMain so early dump paths see daemon mode.
void QnxBerryDaemonForceEnable();

// True if daemon mode was force-enabled (switch/env/marker). Unlike
// QnxBerryDaemonEnabled(), this does NOT touch CommandLine, so it is safe to
// call from main() before ContentMain initializes the global CommandLine.
bool QnxBerryDaemonForced();

void QnxBerryDaemonSetRenderCompleteEvent(WaitableEvent* event);

// Emit rendered HTML on stdout using the berry-daemon framing protocol, then
// either exit (one-shot dump-dom) or signal completion (berry-daemon mode).
void QnxBerryDaemonEmitHtml(base::span<const char> html);

// Emit an error response (berry-daemon mode only).
void QnxBerryDaemonEmitError(const char* message);

#else

inline bool QnxBerryDaemonEnabled() {
  return false;
}

#endif  // BUILDFLAG(IS_QNX)

}  // namespace base

#endif  // BASE_QNX_BERRY_DAEMON_H_
