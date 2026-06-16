#ifndef BASE_QNX_TRACE_H_
#define BASE_QNX_TRACE_H_

#include "build/build_config.h"

#if BUILDFLAG(IS_QNX)

#include <cstdint>
#include <cstdio>
#include <unistd.h>

extern bool g_qnx_trace_enabled;

namespace base {
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

// For use in comma expressions: (QNX_TRACE_THEN("msg", value)) traces then
// returns value.
#define QNX_TRACE_THEN(msg, result) \
  ([&]() -> decltype(auto) { QNX_TRACE_MSG(msg); return (result); }())

#else

#define QNX_TRACE_MSG(msg) ((void)0)
#define QNX_TRACE_FMT(fmt, ...) ((void)0)
#define QNX_TRACE_THEN(msg, result) (result)

#endif

#endif  // BASE_QNX_TRACE_H_
