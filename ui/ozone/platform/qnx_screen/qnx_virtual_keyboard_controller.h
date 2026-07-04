// Copyright 2025 SW7FT. All rights reserved.
// BlackBerry 10 BPS virtual keyboard integration for Passport touch meta strip.

#ifndef UI_OZONE_PLATFORM_QNX_SCREEN_QNX_VIRTUAL_KEYBOARD_CONTROLLER_H_
#define UI_OZONE_PLATFORM_QNX_SCREEN_QNX_VIRTUAL_KEYBOARD_CONTROLLER_H_

#include "base/component_export.h"
#include "base/memory/raw_ptr.h"
#include "base/observer_list.h"
#include "ui/base/ime/virtual_keyboard_controller.h"
#include "ui/base/ime/virtual_keyboard_controller_observer.h"

struct bps_event_t;

namespace ui {

class InputMethodBase;

// Forwards BPS virtual-keyboard / navigator-keyboard events to the active
// QnxVirtualKeyboardController instance (if any).
void QnxDispatchVirtualKeyboardBpsEvent(bps_event_t* event);

// Shows/hides the BB10 virtual keyboard chrome (meta strip + touch keys) via
// BPS virtualkeyboard_* when Chromium requests IME visibility on text focus.
class QnxVirtualKeyboardController : public VirtualKeyboardController {
 public:
  explicit QnxVirtualKeyboardController(InputMethodBase* input_method);
  ~QnxVirtualKeyboardController() override;

  QnxVirtualKeyboardController(const QnxVirtualKeyboardController&) = delete;
  QnxVirtualKeyboardController& operator=(const QnxVirtualKeyboardController&) =
      delete;

  // Called from QnxScreenEventSource when a BPS virtual-keyboard or navigator
  // keyboard event arrives.
  void HandleBpsEvent(bps_event_t* event);

  // VirtualKeyboardController overrides.
  bool DisplayVirtualKeyboard() override;
  void DismissVirtualKeyboard() override;
  void AddObserver(VirtualKeyboardControllerObserver* observer) override;
  void RemoveObserver(VirtualKeyboardControllerObserver* observer) override;
  bool IsKeyboardVisible() override;

 private:
  void UpdateKeyboardOptions();
  void UpdateKeyboardHeight(int height_px);
  void NotifyVisible();
  void NotifyHidden();

  raw_ptr<InputMethodBase> input_method_;
  base::ObserverList<VirtualKeyboardControllerObserver>::Unchecked observers_;
  bool visible_ = false;
  int keyboard_height_px_ = 0;
};

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_QNX_SCREEN_QNX_VIRTUAL_KEYBOARD_CONTROLLER_H_
