// Copyright 2025 SW7FT. All rights reserved.

#include "ui/ozone/platform/qnx_screen/qnx_input_method.h"

#include <stdint.h>

#include "ui/base/ime/text_input_client.h"
#include "ui/events/event.h"
#include "ui/events/types/event_type.h"
#include "ui/ozone/platform/qnx_screen/qnx_virtual_keyboard_controller.h"

namespace ui {

QnxInputMethod::QnxInputMethod(ImeKeyEventDispatcher* ime_key_event_dispatcher)
    : InputMethodBase(
          ime_key_event_dispatcher,
          std::make_unique<QnxVirtualKeyboardController>(this)) {}

QnxInputMethod::~QnxInputMethod() = default;

ui::EventDispatchDetails QnxInputMethod::DispatchKeyEvent(ui::KeyEvent* event) {
  DCHECK(event->type() == ET_KEY_PRESSED || event->type() == ET_KEY_RELEASED);

  if (!GetTextInputClient())
    return DispatchKeyEventPostIME(event);

  ui::EventDispatchDetails dispatch_details = DispatchKeyEventPostIME(event);
  if (!event->stopped_propagation() && !dispatch_details.dispatcher_destroyed &&
      event->type() == ET_KEY_PRESSED && GetTextInputClient()) {
    const uint16_t ch = event->GetCharacter();
    if (ch) {
      GetTextInputClient()->InsertChar(*event);
      event->StopPropagation();
    }
  }
  return dispatch_details;
}

void QnxInputMethod::OnCaretBoundsChanged(const TextInputClient* client) {}

void QnxInputMethod::CancelComposition(const TextInputClient* client) {}

bool QnxInputMethod::IsCandidatePopupOpen() const {
  return false;
}

void QnxInputMethod::OnTextInputTypeChanged(TextInputClient* client) {
  InputMethodBase::OnTextInputTypeChanged(client);
  if (!IsTextInputClientFocused(client))
    return;

  auto* vk = GetVirtualKeyboardController();
  if (!vk)
    return;

  if (IsTextInputTypeNone()) {
    vk->DismissVirtualKeyboard();
    return;
  }

  if (vk->IsKeyboardVisible())
    vk->DisplayVirtualKeyboard();
}

void QnxInputMethod::OnDidChangeFocusedClient(TextInputClient* focused_before,
                                                TextInputClient* focused) {
  InputMethodBase::OnDidChangeFocusedClient(focused_before, focused);

  auto* vk = GetVirtualKeyboardController();
  if (!vk)
    return;

  if (!focused || IsTextInputTypeNone())
    vk->DismissVirtualKeyboard();
}

}  // namespace ui
