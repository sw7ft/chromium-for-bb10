// Copyright 2025 SW7FT. All rights reserved.
// Event source for QNX Screen (touch, keyboard) via screen_get_event

#ifndef UI_OZONE_PLATFORM_QNX_SCREEN_QNX_SCREEN_EVENT_SOURCE_H_
#define UI_OZONE_PLATFORM_QNX_SCREEN_QNX_SCREEN_EVENT_SOURCE_H_

#include <screen/screen.h>

#include "base/memory/raw_ptr.h"
#include "base/timer/timer.h"
#include "ui/events/platform/platform_event_source.h"

namespace ui {

class QnxScreenWindowManager;

class QnxScreenEventSource : public PlatformEventSource {
 public:
  QnxScreenEventSource(screen_context_t ctx,
                       QnxScreenWindowManager* window_manager);
  ~QnxScreenEventSource() override;

 private:
  void PollEvents();
  void ProcessEvent(screen_event_t event);
  void ProcessTouchEvent(screen_event_t event, int type);
  void ProcessKeyboardEvent(screen_event_t event);

  screen_context_t ctx_;
  screen_event_t event_ = nullptr;
  raw_ptr<QnxScreenWindowManager> window_manager_;
  base::RepeatingTimer poll_timer_;
};

}  // namespace ui

#endif
