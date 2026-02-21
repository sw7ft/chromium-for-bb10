#include "ui/ozone/platform/qnx_screen/qnx_screen_event_source.h"
#include <screen/screen.h>
#include <unistd.h>
#include "base/time/time.h"
#include "ui/events/event.h"
#include "ui/events/types/event_type.h"
#include "ui/ozone/platform/qnx_screen/qnx_screen_window.h"
#include "ui/ozone/platform/qnx_screen/qnx_screen_window_manager.h"

namespace ui {

QnxScreenEventSource::QnxScreenEventSource(screen_context_t ctx,
                                           QnxScreenWindowManager* wm)
    : ctx_(ctx), window_manager_(wm) {
  screen_create_event(&event_);
  poll_timer_.Start(FROM_HERE, base::Milliseconds(8),
                    base::BindRepeating(&QnxScreenEventSource::PollEvents,
                                        base::Unretained(this)));
}

QnxScreenEventSource::~QnxScreenEventSource() {
  poll_timer_.Stop();
  if (event_) screen_destroy_event(event_);
}

void QnxScreenEventSource::PollEvents() {
  while (true) {
    if (screen_get_event(ctx_, event_, 0) != 0) break;
    int type = SCREEN_EVENT_NONE;
    screen_get_event_property_iv(event_, SCREEN_PROPERTY_TYPE, &type);
    if (type == SCREEN_EVENT_NONE) break;
    ProcessEvent(event_);
  }
}

void QnxScreenEventSource::ProcessEvent(screen_event_t ev) {
  int type = SCREEN_EVENT_NONE;
  screen_get_event_property_iv(ev, SCREEN_PROPERTY_TYPE, &type);
  switch (type) {
    case SCREEN_EVENT_MTOUCH_TOUCH:
    case SCREEN_EVENT_MTOUCH_MOVE:
    case SCREEN_EVENT_MTOUCH_RELEASE:
      ProcessTouchEvent(ev, type);
      break;
    case SCREEN_EVENT_KEYBOARD:
      ProcessKeyboardEvent(ev);
      break;
    default:
      break;
  }
}

void QnxScreenEventSource::ProcessTouchEvent(screen_event_t ev, int type) {
  int pos[2] = {0, 0};
  screen_get_event_property_iv(ev, SCREEN_PROPERTY_POSITION, pos);
  EventType et = EventType::kTouchReleased;
  if (type == SCREEN_EVENT_MTOUCH_TOUCH) et = EventType::kTouchPressed;
  else if (type == SCREEN_EVENT_MTOUCH_MOVE) et = EventType::kTouchMoved;
  int tid = 0;
  screen_get_event_property_iv(ev, SCREEN_PROPERTY_TOUCH_ID, &tid);
  PointerDetails details(EventPointerType::kTouch, tid, 1.0f, 1.0f, 0.0f);
  TouchEvent touch(et, gfx::Point(pos[0], pos[1]),
                   base::TimeTicks::Now(), details);
  DispatchEvent(&touch);
}

void QnxScreenEventSource::ProcessKeyboardEvent(screen_event_t ev) {
  int flags = 0;
  screen_get_event_property_iv(ev, SCREEN_PROPERTY_FLAGS, &flags);
  int sym = 0;
  screen_get_event_property_iv(ev, SCREEN_PROPERTY_KEY_SYM, &sym);
  bool press = (flags & 1);
  EventType et = press ? EventType::kKeyPressed : EventType::kKeyReleased;
  KeyboardCode vk = static_cast<KeyboardCode>(sym & 0xFF);
  KeyEvent key(et, vk, 0);
  DispatchEvent(&key);
}

}  // namespace ui
