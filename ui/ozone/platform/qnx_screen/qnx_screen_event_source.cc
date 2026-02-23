#include "ui/ozone/platform/qnx_screen/qnx_screen_event_source.h"
#include <screen/screen.h>
#include <unistd.h>
#include "base/time/time.h"
#include "ui/events/event.h"
#include "ui/events/types/event_type.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/ozone/platform/qnx_screen/qnx_screen_input_callback.h"
#include "ui/ozone/platform/qnx_screen/qnx_screen_overlay_callback.h"
#include "ui/ozone/platform/qnx_screen/qnx_screen_repaint.h"
#include "ui/ozone/platform/qnx_screen/qnx_screen_window.h"
#include "ui/ozone/platform/qnx_screen/qnx_screen_window_manager.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkSurface.h"

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

  if (ConsumeQnxScreenRepaintRequest())
    RepaintToolbar();
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

  auto touch_cb = GetQnxScreenTouchCallback();
  if (touch_cb) {
    int cb_type = 2;
    if (type == SCREEN_EVENT_MTOUCH_TOUCH) cb_type = 0;
    else if (type == SCREEN_EVENT_MTOUCH_MOVE) cb_type = 1;
    if (touch_cb(cb_type, pos[0], pos[1]))
      return;
  }

  EventType et = ET_TOUCH_RELEASED;
  if (type == SCREEN_EVENT_MTOUCH_TOUCH) et = ET_TOUCH_PRESSED;
  else if (type == SCREEN_EVENT_MTOUCH_MOVE) et = ET_TOUCH_MOVED;
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

  auto key_cb = GetQnxScreenKeyCallback();
  if (key_cb && key_cb(sym, press))
    return;

  EventType et = press ? ET_KEY_PRESSED : ET_KEY_RELEASED;
  KeyboardCode vk = static_cast<KeyboardCode>(sym & 0xFF);
  KeyEvent key(et, vk, 0);
  DispatchEvent(&key);
}

void QnxScreenEventSource::RepaintToolbar() {
  auto overlay_cb = GetQnxScreenOverlayPaintCallback();
  if (!overlay_cb)
    return;

  // Find the first window and repaint the toolbar area directly
  auto* window = window_manager_->GetFirstWindow();
  if (!window || !window->screen_window())
    return;

  screen_buffer_t buf[2];
  int rc = screen_get_window_property_pv(window->screen_window(),
                                         SCREEN_PROPERTY_RENDER_BUFFERS,
                                         (void**)buf);
  if (rc != 0)
    return;

  void* ptr = nullptr;
  screen_get_buffer_property_pv(buf[0], SCREEN_PROPERTY_POINTER, &ptr);
  if (!ptr)
    return;

  int stride = 0;
  screen_get_buffer_property_iv(buf[0], SCREEN_PROPERTY_STRIDE, &stride);

  int buf_size[2] = {0, 0};
  screen_get_buffer_property_iv(buf[0], SCREEN_PROPERTY_BUFFER_SIZE, buf_size);
  if (buf_size[0] <= 0 || buf_size[1] <= 0)
    return;

  // Create a Skia surface wrapping the screen buffer for toolbar area only
  constexpr int kToolbarHeight = 56;
  SkImageInfo info = SkImageInfo::MakeN32Premul(buf_size[0], kToolbarHeight);
  auto surface = SkSurfaces::WrapPixels(info, ptr, stride);
  if (!surface)
    return;

  overlay_cb(surface->getCanvas());

  int dirty[4] = {0, 0, buf_size[0], kToolbarHeight};
  screen_post_window(window->screen_window(), buf[0], 1, dirty, 0);
}

}  // namespace ui
