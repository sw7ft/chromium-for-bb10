// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/browser_main.h"

#include <memory>

#include "base/debug/alias.h"
#include "base/process/current_process.h"
#include "base/time/time.h"
#include "base/trace_event/trace_event.h"
#include "content/browser/browser_main_runner_impl.h"
#include "content/common/content_constants_internal.h"

#if BUILDFLAG(IS_QNX)
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#endif

namespace content {

// Main routine for running as the Browser process.
int BrowserMain(MainFunctionParams parameters) {
  TRACE_EVENT_INSTANT0("startup", "BrowserMain", TRACE_EVENT_SCOPE_THREAD);
#if BUILDFLAG(IS_QNX)
  write(2, "QNX:BrowserMain:1 entry\n", 24);
#endif

  base::CurrentProcess::GetInstance().SetProcessType(
      base::CurrentProcessType::PROCESS_BROWSER);
  base::trace_event::TraceLog::GetInstance()->SetProcessSortIndex(
      kTraceEventBrowserProcessSortIndex);
#if BUILDFLAG(IS_QNX)
  write(2, "QNX:BrowserMain:2 create\n", 25);
#endif

  std::unique_ptr<BrowserMainRunnerImpl> main_runner(
      BrowserMainRunnerImpl::Create());
#if BUILDFLAG(IS_QNX)
  write(2, "QNX:BrowserMain:3 init\n", 23);
#endif

  int exit_code = main_runner->Initialize(std::move(parameters));
#if BUILDFLAG(IS_QNX)
  {
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "QNX:BrowserMain:4 init=%d\n", exit_code);
    write(2, buf, n);
  }
#endif
  if (exit_code >= 0)
    return exit_code;

#if BUILDFLAG(IS_QNX)
  { char _b[64]; int _n = snprintf(_b, sizeof(_b), "QNX:BrowserMain:5 Run tid=%x\n", (unsigned)pthread_self()); write(2, _b, _n); }
#endif
  exit_code = main_runner->Run();
#if BUILDFLAG(IS_QNX)
  write(2, "QNX:BrowserMain:6 RunDone\n", 26);
#endif

  // Record the time shutdown started in convenient units. This can be compared
  // to times stored in places like ReportThreadHang() and
  // TaskAnnotator::RunTaskImpl() when analyzing hangs.
  const int64_t shutdown_time =
      base::TimeTicks::Now().since_origin().InSeconds();
  base::debug::Alias(&shutdown_time);

  main_runner->Shutdown();

  return exit_code;
}

}  // namespace content
