#ifndef BASE_QNX_PUMP_ACTIVITY_H_
#define BASE_QNX_PUMP_ACTIVITY_H_

#include "build/build_config.h"

#if BUILDFLAG(IS_QNX)

namespace base {

// Marks the process as actively producing frames / handling input so every
// MessagePumpLibevent thread keeps the tight QNX re-scan cap (2ms) instead of
// falling back to the 50ms idle cap mid-gesture.
void MarkQnxProcessPumpActive();

}  // namespace base

#endif  // BUILDFLAG(IS_QNX)

#endif  // BASE_QNX_PUMP_ACTIVITY_H_
