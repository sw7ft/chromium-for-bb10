// Copyright 2025 SW7FT. All rights reserved.

#include "ui/ozone/platform/qnx_screen/qnx_virtual_keyboard_controller.h"

#include <bps/bps.h>
#include <bps/navigator.h>
#include <bps/virtualkeyboard.h>
#include <unistd.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#include "ui/base/ime/input_method_base.h"
#include "ui/base/ime/text_input_client.h"
#include "ui/base/ime/text_input_mode.h"
#include "ui/base/ime/text_input_type.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/ozone/platform/qnx_screen/qnx_screen_sizes.h"

namespace ui {
namespace {

static bool QnxVkDbg() {
  static const bool v = getenv("QNX_KBD_DEBUG") != nullptr;
  return v;
}

static void QnxVkLogf(const char* fmt, ...) {
  if (!QnxVkDbg())
    return;
  char b[160];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  if (n > 0)
    ::write(2, b, n);
}

static virtualkeyboard_layout_t MapLayout(TextInputClient* client) {
  if (!client)
    return VIRTUALKEYBOARD_LAYOUT_WEB;

  switch (client->GetTextInputMode()) {
    case TEXT_INPUT_MODE_URL:
      return VIRTUALKEYBOARD_LAYOUT_URL;
    case TEXT_INPUT_MODE_EMAIL:
      return VIRTUALKEYBOARD_LAYOUT_EMAIL;
    case TEXT_INPUT_MODE_TEL:
      return VIRTUALKEYBOARD_LAYOUT_PHONE;
    case TEXT_INPUT_MODE_NUMERIC:
    case TEXT_INPUT_MODE_DECIMAL:
      return VIRTUALKEYBOARD_LAYOUT_NUMBER;
    case TEXT_INPUT_MODE_SEARCH:
    case TEXT_INPUT_MODE_TEXT:
      return VIRTUALKEYBOARD_LAYOUT_WEB;
    case TEXT_INPUT_MODE_NONE:
    case TEXT_INPUT_MODE_DEFAULT:
      break;
  }

  switch (client->GetTextInputType()) {
    case TEXT_INPUT_TYPE_URL:
      return VIRTUALKEYBOARD_LAYOUT_URL;
    case TEXT_INPUT_TYPE_EMAIL:
      return VIRTUALKEYBOARD_LAYOUT_EMAIL;
    case TEXT_INPUT_TYPE_PASSWORD:
      return VIRTUALKEYBOARD_LAYOUT_PASSWORD;
    case TEXT_INPUT_TYPE_NUMBER:
      return VIRTUALKEYBOARD_LAYOUT_NUMBER;
    case TEXT_INPUT_TYPE_TELEPHONE:
      return VIRTUALKEYBOARD_LAYOUT_PHONE;
    case TEXT_INPUT_TYPE_SEARCH:
      return VIRTUALKEYBOARD_LAYOUT_WEB;
    default:
      return VIRTUALKEYBOARD_LAYOUT_WEB;
  }
}

static virtualkeyboard_enter_t MapEnter(TextInputClient* client) {
  if (!client)
    return VIRTUALKEYBOARD_ENTER_DEFAULT;

  switch (client->GetTextInputMode()) {
    case TEXT_INPUT_MODE_SEARCH:
      return VIRTUALKEYBOARD_ENTER_SEARCH;
    default:
      break;
  }

  switch (client->GetTextInputType()) {
    case TEXT_INPUT_TYPE_SEARCH:
      return VIRTUALKEYBOARD_ENTER_SEARCH;
    default:
      return VIRTUALKEYBOARD_ENTER_DEFAULT;
  }
}

// Active controller receiving BPS keyboard events (one per InputMethod).
QnxVirtualKeyboardController* g_active_controller = nullptr;

}  // namespace

QnxVirtualKeyboardController::QnxVirtualKeyboardController(
    InputMethodBase* input_method)
    : input_method_(input_method) {
  g_active_controller = this;
}

QnxVirtualKeyboardController::~QnxVirtualKeyboardController() {
  if (g_active_controller == this)
    g_active_controller = nullptr;
}

void QnxVirtualKeyboardController::HandleBpsEvent(bps_event_t* event) {
  if (!event)
    return;

  const int domain = bps_event_get_domain(event);
  if (domain == virtualkeyboard_get_domain()) {
    const int code = bps_event_get_code(event);
    QnxVkLogf("VK:bps code=%d\n", code);
    switch (code) {
      case VIRTUALKEYBOARD_EVENT_VISIBLE:
        visible_ = true;
        UpdateKeyboardHeight(virtualkeyboard_event_get_height(event));
        NotifyVisible();
        break;
      case VIRTUALKEYBOARD_EVENT_INFO:
        UpdateKeyboardHeight(virtualkeyboard_event_get_height(event));
        if (visible_)
          NotifyVisible();
        break;
      case VIRTUALKEYBOARD_EVENT_HIDDEN:
        visible_ = false;
        keyboard_height_px_ = 0;
        NotifyHidden();
        break;
      default:
        break;
    }
    return;
  }

  if (domain == navigator_get_domain()) {
    const int code = bps_event_get_code(event);
    if (code == NAVIGATOR_KEYBOARD_STATE) {
      const int st = navigator_event_get_keyboard_state(event);
      QnxVkLogf("VK:nav state=%d\n", st);
      if (st == NAVIGATOR_KEYBOARD_OPENED) {
        visible_ = true;
        int h = 0;
        if (virtualkeyboard_get_height(&h) == BPS_SUCCESS && h > 0)
          UpdateKeyboardHeight(h);
        else
          NotifyVisible();
      } else if (st == NAVIGATOR_KEYBOARD_CLOSED) {
        visible_ = false;
        keyboard_height_px_ = 0;
        NotifyHidden();
      }
    } else if (code == NAVIGATOR_KEYBOARD_POSITION) {
      const int y = navigator_event_get_keyboard_position(event);
      QnxVkLogf("VK:nav pos y=%d\n", y);
      if (y > 0) {
        int rw = 720, rh = 720, ow = 1440, oh = 1440;
        QnxScreenGetRenderSize(&rw, &rh);
        QnxScreenGetOutputSize(&ow, &oh);
        const int top_render = y * rh / oh;
        const int height_render = rh - top_render;
        if (height_render > 0) {
          keyboard_height_px_ = height_render;
          visible_ = true;
          NotifyVisible();
        }
      }
    }
  }
}

void QnxVirtualKeyboardController::UpdateKeyboardOptions() {
  TextInputClient* client = input_method_ ? input_method_->GetTextInputClient()
                                          : nullptr;
  const virtualkeyboard_layout_t layout = MapLayout(client);
  const virtualkeyboard_enter_t enter = MapEnter(client);
  QnxVkLogf("VK:options layout=%d enter=%d\n", static_cast<int>(layout),
            static_cast<int>(enter));
  // no_help=false keeps the contextual hint / meta button row visible.
  virtualkeyboard_change_options_v2(layout, enter, false);
}

bool QnxVirtualKeyboardController::DisplayVirtualKeyboard() {
  QnxVkLogf("VK:show\n");
  UpdateKeyboardOptions();
  virtualkeyboard_show();

  int h = 0;
  if (virtualkeyboard_get_height(&h) == BPS_SUCCESS && h > 0)
    UpdateKeyboardHeight(h);
  else
    visible_ = true;

  NotifyVisible();
  return true;
}

void QnxVirtualKeyboardController::DismissVirtualKeyboard() {
  if (!visible_)
    return;
  QnxVkLogf("VK:hide\n");
  virtualkeyboard_hide();
  visible_ = false;
  keyboard_height_px_ = 0;
  NotifyHidden();
}

void QnxVirtualKeyboardController::AddObserver(
    VirtualKeyboardControllerObserver* observer) {
  observers_.AddObserver(observer);
}

void QnxVirtualKeyboardController::RemoveObserver(
    VirtualKeyboardControllerObserver* observer) {
  observers_.RemoveObserver(observer);
}

bool QnxVirtualKeyboardController::IsKeyboardVisible() {
  return visible_;
}

void QnxVirtualKeyboardController::UpdateKeyboardHeight(int height_px) {
  if (height_px <= 0)
    return;

  int rw = 720, rh = 720, ow = 1440, oh = 1440;
  QnxScreenGetRenderSize(&rw, &rh);
  QnxScreenGetOutputSize(&ow, &oh);
  // BPS height is in output pixels; map to render/dip space.
  keyboard_height_px_ = height_px * rh / oh;
  if (keyboard_height_px_ <= 0)
    keyboard_height_px_ = height_px;
}

void QnxVirtualKeyboardController::NotifyVisible() {
  int rw = 720, rh = 720;
  QnxScreenGetRenderSize(&rw, &rh);

  const int top = (keyboard_height_px_ > 0 && keyboard_height_px_ < rh)
                      ? rh - keyboard_height_px_
                      : rh / 2;
  const int kb_h = (keyboard_height_px_ > 0) ? keyboard_height_px_ : rh / 3;
  const gfx::Rect keyboard_rect(0, top, rw, kb_h);

  if (input_method_)
    input_method_->SetVirtualKeyboardBounds(keyboard_rect);

  for (auto& observer : observers_)
    observer.OnKeyboardVisible(keyboard_rect);
}

void QnxVirtualKeyboardController::NotifyHidden() {
  if (input_method_)
    input_method_->SetVirtualKeyboardBounds(gfx::Rect());

  for (auto& observer : observers_)
    observer.OnKeyboardHidden();
}

void QnxDispatchVirtualKeyboardBpsEvent(bps_event_t* event) {
  if (g_active_controller)
    g_active_controller->HandleBpsEvent(event);
}

}  // namespace ui
