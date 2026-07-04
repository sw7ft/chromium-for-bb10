#ifndef BASE_QNX_TRACE_H_
#define BASE_QNX_TRACE_H_

#include "build/build_config.h"

#if BUILDFLAG(IS_QNX)

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <unistd.h>

extern bool g_qnx_trace_enabled;

namespace base {
// Monotonic wall-clock milliseconds, readable from any thread without pulling in
// base::TimeTicks. Used to correlate cross-thread nav timing (IO-thread network
// vs UI-thread response delivery) in BerryNav logs.
inline long long QnxNowMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// Returns |s| if it is a plausibly-valid C-string pointer, else a placeholder.
// QNX libc printf/snprintf does NOT tolerate a null/garbage %s argument (it
// calls strlen() and faults), unlike glibc. Mojo Connector/endpoint
// interface_name_ pointers can be dangling or uninitialized during teardown or
// cross-thread races, so tracing them via %s would crash the process. Guard all
// QNX_TRACE %s arguments that come from such pointers with this helper so that
// enabling --qnx-trace never itself induces a crash.
inline const char* QnxSafeStr(const char* s) {
  uintptr_t v = reinterpret_cast<uintptr_t>(s);
  if (v < 0x10000u || v > 0x7fffffffu)
    return "<bad>";
  return s;
}

inline bool QnxNavLogEnabled() {
  static const bool on = []() {
    if (getenv("QNX_NAV_DEBUG"))
      return true;
    return access("/accounts/1000/shared/misc/berry-nav.debug", F_OK) == 0 ||
           access("/accounts/1000/shared/misc/berry-kbd.debug", F_OK) == 0 ||
           access("/accounts/1000/shared/misc/berry-video.debug", F_OK) == 0 ||
           access("/accounts/1000/shared/misc/berry-decode.debug", F_OK) == 0;
  }();
  return on;
}

// Verbose GL/ozone bring-up (CreateGLContext, CreateViewGLSurface, etc.).
// Off by default — each fprintf on BB10 costs measurable load time.
inline bool QnxGlLogEnabled() {
  static const bool on = []() {
    if (getenv("QNX_GL_DEBUG"))
      return true;
    return access("/accounts/1000/shared/misc/berry-kbd.debug", F_OK) == 0;
  }();
  return on;
}

// Per-second frame/pump timing (berry-fps.enable or QNX_FPS=1). Shared by the
// present path and the message pump re-scan cap diagnostics.
inline bool QnxFpsLogEnabled() {
  static const bool on = []() {
    const char* e = getenv("QNX_FPS");
    if (e && e[0] == '1')
      return true;
    return access("/accounts/1000/shared/misc/berry-fps.enable", F_OK) == 0;
  }();
  return on;
}

// One-shot post-decode body/header probe for "transport vs gatekeeping" diagnosis.
// touch /accounts/1000/shared/misc/berry-decode.debug
inline bool QnxDecodeProbeEnabled() {
  static const bool on =
      access("/accounts/1000/shared/misc/berry-decode.debug", F_OK) == 0;
  return on;
}
}  // namespace base

#define QNX_TRACE_MSG(msg)                       \
  do {                                           \
    if (g_qnx_trace_enabled) {                   \
      const char _qm[] = msg;                    \
      ::write(2, _qm, sizeof(_qm) - 1);         \
    }                                            \
  } while (0)

#define QNX_TRACE_FMT(fmt, ...)                  \
  do {                                           \
    if (g_qnx_trace_enabled) {                   \
      char _qm[256];                             \
      int _qn = snprintf(_qm, sizeof(_qm),      \
                         fmt, __VA_ARGS__);       \
      if (_qn > 0)                               \
        ::write(2, _qm, _qn);                    \
    }                                            \
  } while (0)

#define QNX_TRACE_THEN(msg, result) \
  ([&]() -> decltype(auto) { QNX_TRACE_MSG(msg); return (result); }())

#define QNX_NAV_LOG(msg)                       \
  do {                                         \
    if (base::QnxNavLogEnabled()) {            \
      const char _qn[] = msg;                  \
      ::write(2, _qn, sizeof(_qn) - 1);       \
    }                                          \
  } while (0)

#define QNX_NAV_LOG_FMT(fmt, ...)              \
  do {                                         \
    if (base::QnxNavLogEnabled()) {            \
      char _qn[512];                           \
      int _ql = snprintf(_qn, sizeof(_qn), fmt, __VA_ARGS__); \
      if (_ql > 0)                             \
        ::write(2, _qn, _ql);                  \
    }                                          \
  } while (0)

#define QNX_DECODE_PROBE_FMT(fmt, ...)           \
  do {                                           \
    if (base::QnxDecodeProbeEnabled()) {         \
      char _qd[512];                             \
      int _ql = snprintf(_qd, sizeof(_qd), fmt, __VA_ARGS__); \
      if (_ql > 0)                               \
        ::write(2, _qd, _ql);                    \
    }                                            \
  } while (0)

#define QNX_GL_LOG(fmt, ...)                     \
  do {                                           \
    if (base::QnxGlLogEnabled()) {               \
      fprintf(stderr, fmt, __VA_ARGS__);         \
    }                                            \
  } while (0)

#define QNX_GL_LOG_MSG(msg)                      \
  do {                                           \
    if (base::QnxGlLogEnabled())                 \
      fprintf(stderr, "%s", msg);                \
  } while (0)

#else

#define QNX_TRACE_MSG(msg) ((void)0)
#define QNX_TRACE_FMT(fmt, ...) ((void)0)
#define QNX_TRACE_THEN(msg, result) (result)
#define QNX_NAV_LOG(msg) ((void)0)
#define QNX_NAV_LOG_FMT(fmt, ...) ((void)0)
#define QNX_DECODE_PROBE_FMT(fmt, ...) ((void)0)
#define QNX_GL_LOG(fmt, ...) ((void)0)
#define QNX_GL_LOG_MSG(msg) ((void)0)

#endif

#endif  // BASE_QNX_TRACE_H_
