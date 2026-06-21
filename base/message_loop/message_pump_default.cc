// Copyright 2006-2008 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/message_loop/message_pump_default.h"

#include "base/auto_reset.h"
#include "base/logging.h"
#include "base/synchronization/waitable_event.h"
#include "base/time/time.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_QNX)
#include <pthread.h>

#include "base/qnx_trace.h"
#endif

#if BUILDFLAG(IS_APPLE)
#include <mach/thread_policy.h>

#include "base/apple/mach_logging.h"
#include "base/apple/scoped_mach_port.h"
#include "base/apple/scoped_nsautorelease_pool.h"
#include "base/threading/threading_features.h"
#endif

namespace base {

MessagePumpDefault::MessagePumpDefault()
    : keep_running_(true),
      event_(WaitableEvent::ResetPolicy::AUTOMATIC,
             WaitableEvent::InitialState::NOT_SIGNALED) {
  event_.declare_only_used_while_idle();
}

MessagePumpDefault::~MessagePumpDefault() = default;

void MessagePumpDefault::Run(Delegate* delegate) {
  AutoReset<bool> auto_reset_keep_running(&keep_running_, true);

  for (;;) {
#if BUILDFLAG(IS_APPLE)
    apple::ScopedNSAutoreleasePool autorelease_pool;
#endif

    Delegate::NextWorkInfo next_work_info = delegate->DoWork();
    bool has_more_immediate_work = next_work_info.is_immediate();
#if BUILDFLAG(IS_QNX)
    QNX_TRACE_FMT("QNX:MPD:DoWork tid=%x p=%p imm=%d\n",
                  (unsigned)pthread_self(), (void*)this,
                  (int)has_more_immediate_work);
#endif
    if (!keep_running_)
      break;

    if (has_more_immediate_work)
      continue;

    has_more_immediate_work = delegate->DoIdleWork();
    if (!keep_running_)
      break;

    if (has_more_immediate_work)
      continue;

    if (next_work_info.delayed_run_time.is_max()) {
#if BUILDFLAG(IS_QNX)
      QNX_TRACE_FMT("QNX:MPD:Wait tid=%x p=%p inf=1\n",
                    (unsigned)pthread_self(), (void*)this);
      // QNX: never park infinitely. Cross-thread WaitableEvent::Signal() can be
      // lost on QNX (see deploy/HARDENING.md idle-wakeup stall), which wedges
      // the renderer main thread so a browser->renderer associated Mojo message
      // (e.g. NavigationClient::CommitNavigation) posted to this thread's queue
      // is never serviced. Poll on a bounded TimedWait so DoWork() re-runs and
      // drains any queued task even if its wakeup Signal was dropped.
      event_.TimedWait(Milliseconds(50));
      QNX_TRACE_FMT("QNX:MPD:Wake tid=%x p=%p inf=1\n",
                    (unsigned)pthread_self(), (void*)this);
#else
      event_.Wait();
#endif
    } else {
#if BUILDFLAG(IS_QNX)
      QNX_TRACE_FMT("QNX:MPD:Wait tid=%x p=%p inf=0 ms=%lld\n",
                    (unsigned)pthread_self(), (void*)this,
                    (long long)next_work_info.remaining_delay().InMilliseconds());
#endif
      event_.TimedWait(next_work_info.remaining_delay());
#if BUILDFLAG(IS_QNX)
      QNX_TRACE_FMT("QNX:MPD:Wake tid=%x p=%p inf=0\n",
                    (unsigned)pthread_self(), (void*)this);
#endif
    }
    // Since event_ is auto-reset, we don't need to do anything special here
    // other than service each delegate method.
  }
}

void MessagePumpDefault::Quit() {
  keep_running_ = false;
}

void MessagePumpDefault::ScheduleWork() {
  // Since this can be called on any thread, we need to ensure that our Run
  // loop wakes up.
#if BUILDFLAG(IS_QNX)
  QNX_TRACE_FMT("QNX:MPD:Sig tid=%x p=%p\n",
                (unsigned)pthread_self(), (void*)this);
#endif
  event_.Signal();
}

void MessagePumpDefault::ScheduleDelayedWork(
    const Delegate::NextWorkInfo& next_work_info) {
  // Since this is always called from the same thread as Run(), there is nothing
  // to do as the loop is already running. It will wait in Run() with the
  // correct timeout when it's out of immediate tasks.
  // TODO(gab): Consider removing ScheduleDelayedWork() when all pumps function
  // this way (bit.ly/merge-message-pump-do-work).
}

}  // namespace base
