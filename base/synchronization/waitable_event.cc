// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/synchronization/waitable_event.h"

#include "base/threading/scoped_blocking_call.h"
#include "base/trace_event/base_tracing.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_QNX)
#include <pthread.h>
#include <cstdio>
#include <unistd.h>
#endif

namespace base {

void WaitableEvent::Signal() {
  // Must be ordered before SignalImpl() to guarantee it's emitted before the
  // matching TerminatingFlow in TimedWait().
  if (!only_used_while_idle_) {
    TRACE_EVENT_INSTANT("wakeup.flow", "WaitableEvent::Signal",
                        perfetto::Flow::FromPointer(this));
  }
  SignalImpl();
}

void WaitableEvent::Wait() {
#if BUILDFLAG(IS_QNX)
  // ALWAYS-ON, low-volume: log only *non-idle* infinite waits (real synchronous
  // blocks; pump/threadpool idle waits set only_used_while_idle_ and are
  // skipped). The caller return address names exactly who is blocking; an
  // unmatched WE:wait for a given tid is a thread stuck in a sync wait.
  if (!only_used_while_idle_) {
    char _qb[128];
    int _qn = snprintf(_qb, sizeof(_qb),
                       "QNX:WE:wait tid=%x ev=%p ra0=%p\n",
                       (unsigned)pthread_self(), (void*)this,
                       __builtin_return_address(0));
    if (_qn > 0)
      ::write(2, _qb, _qn);
  }
#endif
  const bool result = TimedWait(TimeDelta::Max());
#if BUILDFLAG(IS_QNX)
  if (!only_used_while_idle_) {
    char _qb[96];
    int _qn = snprintf(_qb, sizeof(_qb), "QNX:WE:ret tid=%x ev=%p\n",
                       (unsigned)pthread_self(), (void*)this);
    if (_qn > 0)
      ::write(2, _qb, _qn);
  }
#endif
  DCHECK(result) << "TimedWait() should never fail with infinite timeout";
}

bool WaitableEvent::TimedWait(TimeDelta wait_delta) {
  if (wait_delta <= TimeDelta())
    return IsSignaled();

  // Consider this thread blocked for scheduling purposes. Ignore this for
  // non-blocking WaitableEvents.
  absl::optional<internal::ScopedBlockingCallWithBaseSyncPrimitives>
      scoped_blocking_call;
  if (!only_used_while_idle_) {
    scoped_blocking_call.emplace(FROM_HERE, BlockingType::MAY_BLOCK);
  }

  const bool result = TimedWaitImpl(wait_delta);

  if (result && !only_used_while_idle_) {
    TRACE_EVENT_INSTANT("wakeup.flow", "WaitableEvent::Wait Complete",
                        perfetto::TerminatingFlow::FromPointer(this));
  }

  return result;
}

}  // namespace base
