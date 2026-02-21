// Copyright 2025 SW7FT. All rights reserved.
// Ozone platform backend for QNX Screen (BlackBerry 10 / Passport)

#include "ui/ozone/platform/qnx_screen/ozone_platform_qnx_screen.h"

#include <screen/screen.h>
#include <memory>
#include <unistd.h>

#include "base/no_destructor.h"
#include "ui/base/cursor/cursor_factory.h"
#include "ui/base/ime/input_method_minimal.h"
#include "ui/display/types/native_display_delegate.h"
#include "ui/events/ozone/layout/keyboard_layout_engine_manager.h"
#include "ui/events/ozone/layout/stub/stub_keyboard_layout_engine.h"
#include "ui/ozone/common/bitmap_cursor_factory.h"
#include "ui/ozone/common/stub_overlay_manager.h"
#include "ui/ozone/platform/qnx_screen/qnx_screen_event_source.h"
#include "ui/ozone/platform/qnx_screen/qnx_screen_surface_factory.h"
#include "ui/ozone/platform/qnx_screen/qnx_screen_window.h"
#include "ui/ozone/platform/qnx_screen/qnx_screen_window_manager.h"
#include "ui/ozone/public/gpu_platform_support_host.h"
#include "ui/ozone/public/input_controller.h"
#include "ui/ozone/public/ozone_platform.h"
#include "ui/ozone/public/system_input_injector.h"
#include "ui/platform_window/platform_window_init_properties.h"

namespace ui {

namespace {

class OzonePlatformQnxScreen : public OzonePlatform {
 public:
  OzonePlatformQnxScreen() = default;
  ~OzonePlatformQnxScreen() override {
    if (screen_ctx_) {
      screen_destroy_context(screen_ctx_);
      screen_ctx_ = nullptr;
    }
  }

  SurfaceFactoryOzone* GetSurfaceFactoryOzone() override {
    return surface_factory_.get();
  }

  OverlayManagerOzone* GetOverlayManager() override {
    return overlay_manager_.get();
  }

  CursorFactory* GetCursorFactory() override {
    return cursor_factory_.get();
  }

  InputController* GetInputController() override {
    return input_controller_.get();
  }

  GpuPlatformSupportHost* GetGpuPlatformSupportHost() override {
    return gpu_platform_support_host_.get();
  }

  std::unique_ptr<SystemInputInjector> CreateSystemInputInjector() override {
    return nullptr;
  }

  std::unique_ptr<PlatformWindow> CreatePlatformWindow(
      PlatformWindowDelegate* delegate,
      PlatformWindowInitProperties properties) override {
    return std::make_unique<QnxScreenWindow>(
        delegate, window_manager_.get(), screen_ctx_, properties.bounds);
  }

  std::unique_ptr<display::NativeDisplayDelegate>
  CreateNativeDisplayDelegate() override {
    return nullptr;
  }

  std::unique_ptr<InputMethod> CreateInputMethod(
      ImeKeyEventDispatcher* dispatcher,
      gfx::AcceleratedWidget widget) override {
    return std::make_unique<InputMethodMinimal>(dispatcher);
  }

  bool InitializeUI(const InitParams& params) override {
    int rc = screen_create_context(&screen_ctx_, SCREEN_APPLICATION_CONTEXT);
    if (rc != 0) {
      const char msg[] = "QNX:Ozone: screen_create_context failed\n";
      ::write(2, msg, sizeof(msg) - 1);
      return false;
    }

    window_manager_ = std::make_unique<QnxScreenWindowManager>();
    surface_factory_ =
        std::make_unique<QnxScreenSurfaceFactory>(window_manager_.get());

    event_source_ = std::make_unique<QnxScreenEventSource>(
        screen_ctx_, window_manager_.get());

    keyboard_layout_engine_ = std::make_unique<StubKeyboardLayoutEngine>();
    KeyboardLayoutEngineManager::SetKeyboardLayoutEngine(
        keyboard_layout_engine_.get());

    overlay_manager_ = std::make_unique<StubOverlayManager>();
    input_controller_ = CreateStubInputController();
    cursor_factory_ = std::make_unique<BitmapCursorFactory>();
    gpu_platform_support_host_.reset(CreateStubGpuPlatformSupportHost());

    return true;
  }

  void InitializeGPU(const InitParams& params) override {
    if (!surface_factory_) {
      if (!window_manager_)
        window_manager_ = std::make_unique<QnxScreenWindowManager>();
      surface_factory_ =
          std::make_unique<QnxScreenSurfaceFactory>(window_manager_.get());
    }
  }

 private:
  screen_context_t screen_ctx_ = nullptr;
  std::unique_ptr<KeyboardLayoutEngine> keyboard_layout_engine_;
  std::unique_ptr<QnxScreenWindowManager> window_manager_;
  std::unique_ptr<QnxScreenSurfaceFactory> surface_factory_;
  std::unique_ptr<QnxScreenEventSource> event_source_;
  std::unique_ptr<CursorFactory> cursor_factory_;
  std::unique_ptr<InputController> input_controller_;
  std::unique_ptr<GpuPlatformSupportHost> gpu_platform_support_host_;
  std::unique_ptr<OverlayManagerOzone> overlay_manager_;
};

}  // namespace

OzonePlatform* CreateOzonePlatformQnxScreen() {
  return new OzonePlatformQnxScreen();
}

}  // namespace ui
