// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/renderer/in_process_renderer_thread.h"

#include "build/build_config.h"
#include "base/qnx_trace.h"
#include "content/public/common/content_client.h"
#include "content/public/renderer/content_renderer_client.h"
#include "content/renderer/render_process.h"
#include "content/renderer/render_process_impl.h"
#include "content/renderer/render_thread_impl.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/public/platform/scheduler/web_thread_scheduler.h"

#if BUILDFLAG(IS_ANDROID)
#include "base/android/jni_android.h"
#endif
#if BUILDFLAG(IS_QNX)
#include <unistd.h>
#endif

namespace content {

InProcessRendererThread::InProcessRendererThread(
    const InProcessChildThreadParams& params,
    int32_t renderer_client_id)
    : Thread("Chrome_InProcRendererThread"),
      params_(params),
      renderer_client_id_(renderer_client_id) {}

InProcessRendererThread::~InProcessRendererThread() {
  Stop();
}

void InProcessRendererThread::Init() {
  QNX_TRACE_MSG("QNX:Renderer:1 Init\n");
  content::ContentRendererClient* client = GetContentClient()->renderer();
  if (client) {
    client->PostSandboxInitialized();
  }

#if BUILDFLAG(IS_ANDROID)
  base::android::AttachCurrentThreadWithName(thread_name());
  CHECK(!render_process_);
#endif
  QNX_TRACE_MSG("QNX:Renderer:2 InitBlink\n");
  blink::Platform::InitializeBlink();
  QNX_TRACE_MSG("QNX:Renderer:3 Scheduler\n");
  std::unique_ptr<blink::scheduler::WebThreadScheduler> main_thread_scheduler =
      blink::scheduler::WebThreadScheduler::CreateMainThreadScheduler();

  QNX_TRACE_MSG("QNX:Renderer:4 RenderProc\n");
  render_process_ = RenderProcessImpl::Create();
  QNX_TRACE_MSG("QNX:Renderer:5 RenderThread\n");
  new RenderThreadImpl(params_, renderer_client_id_,
                       std::move(main_thread_scheduler));
  QNX_TRACE_MSG("QNX:Renderer:6 Done\n");
}

void InProcessRendererThread::CleanUp() {
  render_process_.reset();

  // It's a little lame to manually set this flag.  But the single process
  // RendererThread will receive the WM_QUIT.  We don't need to assert on
  // this thread, so just force the flag manually.
  // If we want to avoid this, we could create the InProcRendererThread
  // directly with _beginthreadex() rather than using the Thread class.
  // We used to set this flag in the Init function above. However there
  // other threads like WebThread which are created by this thread
  // which resets this flag. Please see Thread::StartWithOptions. Setting
  // this flag to true in Cleanup works around these problems.
  SetThreadWasQuitProperly(true);
}

base::Thread* CreateInProcessRendererThread(
    const InProcessChildThreadParams& params,
    int32_t renderer_client_id) {
  return new InProcessRendererThread(params, renderer_client_id);
}

}  // namespace content
