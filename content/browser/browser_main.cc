// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/browser_main.h"

#include <memory>

#include "base/debug/alias.h"
#include "base/process/current_process.h"
#include "base/time/time.h"
#include "base/trace_event/trace_event.h"
#include "base/qnx_trace.h"
#include "content/browser/browser_main_runner_impl.h"
#include "content/common/content_constants_internal.h"

#if BUILDFLAG(IS_QNX)
#include <pthread.h>
#endif

namespace content {

// Main routine for running as the Browser process.
int BrowserMain(MainFunctionParams parameters) {
  TRACE_EVENT_INSTANT0("startup", "BrowserMain", TRACE_EVENT_SCOPE_THREAD);
  QNX_TRACE_MSG("QNX:BrowserMain:1 entry\n");

  base::CurrentProcess::GetInstance().SetProcessType(
      base::CurrentProcessType::PROCESS_BROWSER);
  base::trace_event::TraceLog::GetInstance()->SetProcessSortIndex(
      kTraceEventBrowserProcessSortIndex);
  QNX_TRACE_MSG("QNX:BrowserMain:2 create\n");

  std::unique_ptr<BrowserMainRunnerImpl> main_runner(
      BrowserMainRunnerImpl::Create());
  QNX_TRACE_MSG("QNX:BrowserMain:3 init\n");

  int exit_code = main_runner->Initialize(std::move(parameters));
  QNX_TRACE_FMT("QNX:BrowserMain:4 init=%d\n", exit_code);
  if (exit_code >= 0)
    return exit_code;

#if BUILDFLAG(IS_QNX)
  QNX_TRACE_FMT("QNX:BrowserMain:5 Run tid=%x\n", (unsigned)pthread_self());
#endif
  exit_code = main_runner->Run();
  QNX_TRACE_MSG("QNX:BrowserMain:6 RunDone\n");

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
