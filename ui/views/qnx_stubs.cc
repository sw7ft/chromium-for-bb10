// QNX platform stubs for views/UI symbols
#include "build/build_config.h"

#if BUILDFLAG(IS_QNX)

#include <memory>

#include "base/functional/callback.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/views/controls/menu/menu_config.h"

namespace views {

namespace internal {
class NativeWidgetDelegate;
}
class DesktopNativeWidgetAura;

class DesktopWindowTreeHost {
 public:
  static DesktopWindowTreeHost* Create(
      internal::NativeWidgetDelegate* native_widget_delegate,
      DesktopNativeWidgetAura* desktop_native_widget_aura);
};

DesktopWindowTreeHost* DesktopWindowTreeHost::Create(
    internal::NativeWidgetDelegate*,
    DesktopNativeWidgetAura*) {
  return nullptr;
}

void MenuConfig::Init() {}
void MenuConfig::InitPlatformCR2023() {}

}  // namespace views

namespace ui {

class KeyEvent;

class KeyboardHook {
 public:
  static std::unique_ptr<KeyboardHook> CreateModifierKeyboardHook(
      std::optional<base::flat_set<ui::DomCode>> dom_codes,
      unsigned int accelerator_grouping_key,
      base::RepeatingCallback<void(KeyEvent*)> callback);
};

std::unique_ptr<KeyboardHook> KeyboardHook::CreateModifierKeyboardHook(
    std::optional<base::flat_set<ui::DomCode>>,
    unsigned int,
    base::RepeatingCallback<void(KeyEvent*)>) {
  return nullptr;
}

}  // namespace ui

#endif  // BUILDFLAG(IS_QNX)
