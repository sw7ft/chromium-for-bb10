#include "ui/ozone/platform/qnx_screen/qnx_screen_event_source.h"
#include <bps/bps.h>
#include <bps/event.h>
#include <bps/navigator.h>
#include <bps/screen.h>
#include <screen/screen.h>
#include <sys/keycodes.h>
#include <unistd.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include "base/qnx_trace.h"
#include "base/qnx_pump_activity.h"
#include "base/time/time.h"
#include "ui/events/event.h"
#include "ui/events/types/event_type.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/events/keycodes/dom/dom_key.h"
#include "ui/events/keycodes/keyboard_code_conversion.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/ozone/platform/qnx_screen/qnx_screen_sizes.h"
#include "ui/ozone/platform/qnx_screen/qnx_screen_input_callback.h"
#include "ui/ozone/platform/qnx_screen/qnx_screen_overlay_callback.h"
#include "ui/ozone/platform/qnx_screen/qnx_screen_repaint.h"
#include "ui/ozone/platform/qnx_screen/qnx_screen_window.h"
#include "ui/ozone/platform/qnx_screen/qnx_screen_window_manager.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkSurface.h"

namespace ui {

// Lightweight, crash-safe input diagnostic, independent of the heavy --qnx-trace
// firehose (which crashes under load via libc %s formatting). Enable with
// QNX_KBD_DEBUG=1; logs integers only via a single write(2).
static bool QnxKbdDbg() {
  static const bool v = getenv("QNX_KBD_DEBUG") != nullptr;
  return v;
}
static void QnxKbdLogf(const char* fmt, ...) {
  if (!QnxKbdDbg())
    return;
  char b[128];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  if (n > 0)
    ::write(2, b, n);
}

QnxScreenEventSource::QnxScreenEventSource(screen_context_t ctx,
                                           QnxScreenWindowManager* wm)
    : ctx_(ctx), window_manager_(wm) {
  // On BB10 the hardware keyboard is delivered only through BPS/Navigator, not
  // via raw screen_get_event(). Register a BPS channel on this (UI) thread and
  // route both libscreen and navigator events through it. The header explicitly
  // forbids mixing screen_get_event() with BPS, so PollEvents drains via
  // bps_get_event() below. navigator_request_events() is what makes the
  // Navigator treat us as the focused app and route keystrokes here.
  if (bps_initialize() != BPS_SUCCESS)
    QnxKbdLogf("KBD:bps_initialize FAILED\n");
  bps_initialized_ = true;
  if (screen_request_events(ctx_) != BPS_SUCCESS)
    QnxKbdLogf("KBD:screen_request_events FAILED\n");
  if (navigator_request_events(0) != BPS_SUCCESS)
    QnxKbdLogf("KBD:navigator_request_events FAILED\n");
  QnxKbdLogf("KBD:bps registered ok\n");

  // Poll BPS/screen input at 2ms during touch/pinch so multitouch moves reach
  // the gesture recognizer without waiting on a slow periodic tick (was 4ms).
  poll_timer_.Start(FROM_HERE, base::Milliseconds(2),
                    base::BindRepeating(&QnxScreenEventSource::PollEvents,
                                        base::Unretained(this)));
}

QnxScreenEventSource::~QnxScreenEventSource() {
  poll_timer_.Stop();
  if (bps_initialized_) {
    screen_stop_events(ctx_);
    bps_shutdown();
    bps_initialized_ = false;
  }
}

void QnxScreenEventSource::PollEvents() {
  // Liveness: confirm the poll timer is firing on-device (--qnx-trace only).
  static unsigned long s_poll_count = 0;
  if ((s_poll_count++ % 1250) == 0)
    QNX_TRACE_FMT("QNX:Evt: poll alive #%lu\n", s_poll_count);

  // Drain all currently-queued BPS events (non-blocking, timeout 0). libscreen
  // input (touch + hardware keyboard) and navigator lifecycle both arrive here.
  while (true) {
    bps_event_t* event = nullptr;
    if (bps_get_event(&event, 0) != BPS_SUCCESS || !event)
      break;

    base::MarkQnxProcessPumpActive();

    int domain = bps_event_get_domain(event);
    if (domain == screen_get_domain()) {
      screen_event_t se = screen_event_get_event(event);
      int type = SCREEN_EVENT_NONE;
      screen_get_event_property_iv(se, SCREEN_PROPERTY_TYPE, &type);
      QNX_TRACE_FMT("QNX:Evt: got screen event type=%d\n", type);
      QnxKbdLogf("KBD:evt type=%d\n", type);
      ProcessEvent(se);
    } else if (domain == navigator_get_domain()) {
      // We registered with navigator_request_events() so the hardware keyboard
      // is routed to us. That registration also makes us responsible for the
      // orientation handshake: the Navigator sends NAVIGATOR_ORIENTATION_CHECK
      // at launch (and on rotation) and BLOCKS the app window (white screen,
      // then force-kill after a timeout) until we reply. We render a fixed
      // full-display window and manage rotation ourselves via
      // QNX_SCREEN_ROTATION, so we decline rotation and immediately ack.
      int code = bps_event_get_code(event);
      QnxKbdLogf("KBD:nav code=%d\n", code);
      switch (code) {
        case NAVIGATOR_ORIENTATION_CHECK:
          navigator_orientation_check_response(event, false /*will_rotate*/);
          break;
        case NAVIGATOR_ORIENTATION:
          navigator_done_orientation(event);
          break;
        case NAVIGATOR_EXIT: {
          QnxKbdLogf("KBD:nav EXIT\n");
          if (auto cb = GetQnxScreenExitCallback())
            cb();
          break;
        }
        default:
          break;
      }
    }
  }

  // Toolbar updates are composited in PresentCanvas (single present path).
  // RepaintToolbar() posted a second screen_post_window every 4ms and caused
  // flicker that worsened during navigation.
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
  base::MarkQnxProcessPumpActive();
  int pos[2] = {0, 0};
  screen_get_event_property_iv(ev, SCREEN_PROPERTY_POSITION, pos);
  int x = pos[0], y = pos[1];
  QnxScreenMapOutputToRender(pos[0], pos[1], &x, &y);
  QNX_TRACE_FMT("QNX:Touch: type=%d pos=(%d,%d)->(%d,%d)\n", type, pos[0],
                pos[1], x, y);

  auto touch_cb = GetQnxScreenTouchCallback();
  if (touch_cb) {
    int cb_type = 2;
    if (type == SCREEN_EVENT_MTOUCH_TOUCH) cb_type = 0;
    else if (type == SCREEN_EVENT_MTOUCH_MOVE) cb_type = 1;
    NotifyQnxScreenTouchGesture(type != SCREEN_EVENT_MTOUCH_RELEASE,
                                type == SCREEN_EVENT_MTOUCH_MOVE);
    if (touch_cb(cb_type, x, y))
      return;
  }

  NotifyQnxScreenTouchGesture(type != SCREEN_EVENT_MTOUCH_RELEASE,
                              type == SCREEN_EVENT_MTOUCH_MOVE);

  EventType et = ET_TOUCH_RELEASED;
  if (type == SCREEN_EVENT_MTOUCH_TOUCH) et = ET_TOUCH_PRESSED;
  else if (type == SCREEN_EVENT_MTOUCH_MOVE) et = ET_TOUCH_MOVED;
  int tid = 0;
  screen_get_event_property_iv(ev, SCREEN_PROPERTY_TOUCH_ID, &tid);
  PointerDetails details(EventPointerType::kTouch, tid, 1.0f, 1.0f, 0.0f);
  TouchEvent touch(et, gfx::Point(x, y), base::TimeTicks::Now(), details);
  DispatchEvent(&touch);
}

namespace {

struct QnxKeyMap {
  KeyboardCode key_code = VKEY_UNKNOWN;
  DomCode dom_code = DomCode::NONE;
  DomKey dom_key = DomKey::NONE;
  bool valid = false;
};

// Translate a QNX Screen key symbol (sys/keycodes.h) into the Chromium key
// triple. Printable symbols are Unicode code points; special keys live in the
// Unicode private-use area (KEYCODE_PC_KEYS based).
QnxKeyMap MapQnxKey(int sym) {
  QnxKeyMap m;
  switch (sym) {
    case KEYCODE_RETURN:
    case KEYCODE_KP_ENTER:
    case 0x0d:
    case 0x0a:
      m = {VKEY_RETURN, DomCode::ENTER, DomKey::ENTER, true};
      return m;
    case KEYCODE_BACKSPACE:
      m = {VKEY_BACK, DomCode::BACKSPACE, DomKey::BACKSPACE, true};
      return m;
    case KEYCODE_TAB:
      m = {VKEY_TAB, DomCode::TAB, DomKey::TAB, true};
      return m;
    case KEYCODE_ESCAPE:
      m = {VKEY_ESCAPE, DomCode::ESCAPE, DomKey::ESCAPE, true};
      return m;
    case KEYCODE_LEFT:
      m = {VKEY_LEFT, DomCode::ARROW_LEFT, DomKey::ARROW_LEFT, true};
      return m;
    case KEYCODE_RIGHT:
      m = {VKEY_RIGHT, DomCode::ARROW_RIGHT, DomKey::ARROW_RIGHT, true};
      return m;
    case KEYCODE_UP:
      m = {VKEY_UP, DomCode::ARROW_UP, DomKey::ARROW_UP, true};
      return m;
    case KEYCODE_DOWN:
      m = {VKEY_DOWN, DomCode::ARROW_DOWN, DomKey::ARROW_DOWN, true};
      return m;
    case KEYCODE_HOME:
      m = {VKEY_HOME, DomCode::HOME, DomKey::HOME, true};
      return m;
    case KEYCODE_END:
      m = {VKEY_END, DomCode::END, DomKey::END, true};
      return m;
    case KEYCODE_PG_UP:
      m = {VKEY_PRIOR, DomCode::PAGE_UP, DomKey::PAGE_UP, true};
      return m;
    case KEYCODE_PG_DOWN:
      m = {VKEY_NEXT, DomCode::PAGE_DOWN, DomKey::PAGE_DOWN, true};
      return m;
    case KEYCODE_DELETE:
      m = {VKEY_DELETE, DomCode::DEL, DomKey::DEL, true};
      return m;
    case KEYCODE_INSERT:
      m = {VKEY_INSERT, DomCode::INSERT, DomKey::INSERT, true};
      return m;
    default:
      break;
  }

  // Printable BMP character (everything below the private-use area). The symbol
  // already reflects Shift (e.g. 'A' vs 'a'), so carry it as the DomKey so the
  // InputMethod inserts the right text. VKEY is best-effort for shortcuts.
  if (sym >= 0x20 && sym < 0xE000) {
    m.dom_key = DomKey::FromCharacter(sym);
    m.dom_code = UsLayoutDomKeyToDomCode(m.dom_key);
    m.valid = true;
    if (sym >= 'a' && sym <= 'z')
      m.key_code = static_cast<KeyboardCode>(VKEY_A + (sym - 'a'));
    else if (sym >= 'A' && sym <= 'Z')
      m.key_code = static_cast<KeyboardCode>(VKEY_A + (sym - 'A'));
    else if (sym >= '0' && sym <= '9')
      m.key_code = static_cast<KeyboardCode>(VKEY_0 + (sym - '0'));
    else if (sym == ' ')
      m = {VKEY_SPACE, DomCode::SPACE, DomKey::FromCharacter(' '), true};
    else if (m.key_code == VKEY_UNKNOWN && m.dom_code != DomCode::NONE)
      m.key_code = DomCodeToUsLayoutKeyboardCode(m.dom_code);
  }
  return m;
}

}  // namespace

void QnxScreenEventSource::ProcessKeyboardEvent(screen_event_t ev) {
  base::MarkQnxProcessPumpActive();
  int kflags = 0;
  screen_get_event_property_iv(ev, SCREEN_PROPERTY_KEY_FLAGS, &kflags);
  int sym = 0;
  screen_get_event_property_iv(ev, SCREEN_PROPERTY_KEY_SYM, &sym);
  int mods = 0;
  screen_get_event_property_iv(ev, SCREEN_PROPERTY_KEY_MODIFIERS, &mods);

  bool down = (kflags & SCREEN_FLAG_KEY_DOWN) != 0;

  auto key_cb = GetQnxScreenKeyCallback();
  if (key_cb && key_cb(sym, down))
    return;

  QnxKeyMap m = MapQnxKey(sym);
  if (m.valid) {
    if (m.dom_code == DomCode::NONE) {
      if (m.dom_key != DomKey::NONE)
        m.dom_code = UsLayoutDomKeyToDomCode(m.dom_key);
      else if (m.key_code != VKEY_UNKNOWN)
        m.dom_code = UsLayoutKeyboardCodeToDomCode(m.key_code);
    }
    if (m.key_code == VKEY_UNKNOWN && m.dom_code != DomCode::NONE)
      m.key_code = DomCodeToUsLayoutKeyboardCode(m.dom_code);
  }
  QNX_TRACE_FMT("QNX:Key: sym=0x%x down=%d vk=%d mods=0x%x valid=%d\n", sym,
                down ? 1 : 0, m.key_code, mods, m.valid ? 1 : 0);
  QnxKbdLogf("KBD:key sym=0x%x down=%d vk=%d dc=%u mods=0x%x valid=%d\n", sym,
             down ? 1 : 0, m.key_code,
             static_cast<unsigned>(static_cast<uint32_t>(m.dom_code)), mods,
             m.valid ? 1 : 0);
  if (!m.valid)
    return;

  int flags = 0;
  if (mods & KEYMOD_SHIFT)
    flags |= EF_SHIFT_DOWN;
  if (mods & KEYMOD_CTRL)
    flags |= EF_CONTROL_DOWN;
  if (mods & KEYMOD_ALT)
    flags |= EF_ALT_DOWN;

  EventType et = down ? ET_KEY_PRESSED : ET_KEY_RELEASED;
  KeyEvent key(et, m.key_code, m.dom_code, flags, m.dom_key,
               base::TimeTicks::Now());
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
