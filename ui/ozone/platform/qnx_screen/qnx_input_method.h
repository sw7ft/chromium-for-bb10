// Copyright 2025 SW7FT. All rights reserved.
// QNX input method with BB10 virtual keyboard integration.

#ifndef UI_OZONE_PLATFORM_QNX_SCREEN_QNX_INPUT_METHOD_H_
#define UI_OZONE_PLATFORM_QNX_SCREEN_QNX_INPUT_METHOD_H_

#include "ui/base/ime/input_method_base.h"

namespace ui {

// InputMethodMinimal plus a BB10 VirtualKeyboardController for the touch meta
// strip when web form fields are focused via touch.
class QnxInputMethod : public InputMethodBase {
 public:
  explicit QnxInputMethod(ImeKeyEventDispatcher* ime_key_event_dispatcher);

  QnxInputMethod(const QnxInputMethod&) = delete;
  QnxInputMethod& operator=(const QnxInputMethod&) = delete;

  ~QnxInputMethod() override;

  ui::EventDispatchDetails DispatchKeyEvent(ui::KeyEvent* event) override;
  void OnCaretBoundsChanged(const TextInputClient* client) override;
  void CancelComposition(const TextInputClient* client) override;
  bool IsCandidatePopupOpen() const override;
  void OnTextInputTypeChanged(TextInputClient* client) override;

 protected:
  void OnDidChangeFocusedClient(TextInputClient* focused_before,
                                TextInputClient* focused) override;
};

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_QNX_SCREEN_QNX_INPUT_METHOD_H_
