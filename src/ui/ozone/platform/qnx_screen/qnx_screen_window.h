// Copyright 2025 SW7FT. All rights reserved.
// QNX Screen window implementation for Ozone

#ifndef UI_OZONE_PLATFORM_QNX_SCREEN_QNX_SCREEN_WINDOW_H_
#define UI_OZONE_PLATFORM_QNX_SCREEN_QNX_SCREEN_WINDOW_H_

#include <screen/screen.h>

#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/native_widget_types.h"
#include "ui/platform_window/platform_window.h"
#include "ui/platform_window/platform_window_delegate.h"

namespace ui {

class QnxScreenWindowManager;

class QnxScreenWindow : public PlatformWindow {
 public:
  QnxScreenWindow(PlatformWindowDelegate* delegate,
                  QnxScreenWindowManager* manager,
                  screen_context_t ctx,
                  const gfx::Rect& bounds);
  ~QnxScreenWindow() override;

  QnxScreenWindow(const QnxScreenWindow&) = delete;
  QnxScreenWindow& operator=(const QnxScreenWindow&) = delete;

  screen_window_t screen_window() const { return window_; }
  screen_context_t screen_context() const { return ctx_; }

  // PlatformWindow:
  gfx::Rect GetBoundsInPixels() const override;
  void SetBoundsInPixels(const gfx::Rect& bounds) override;
  gfx::Rect GetBoundsInDIP() const override;
  void SetBoundsInDIP(const gfx::Rect& bounds) override;
  void Show(bool inactive = false) override;
  void Hide() override;
  void Close() override;
  bool IsVisible() const override;
  void SetTitle(const std::u16string& title) override;
  void SetCapture() override;
  void ReleaseCapture() override;
  bool HasCapture() const override;
  void SetFullscreen(bool fullscreen, int64_t target_display_id) override;
  void Maximize() override;
  void Minimize() override;
  void Restore() override;
  PlatformWindowState GetPlatformWindowState() const override;
  void Activate() override;
  void Deactivate() override;
  void SetUseNativeFrame(bool use_native_frame) override;
  bool ShouldUseNativeFrame() const override;
  void SetCursor(scoped_refptr<PlatformCursor> cursor) override;
  void MoveCursorTo(const gfx::Point& location) override;
  void ConfineCursorToBounds(const gfx::Rect& bounds) override;
  void SetRestoredBoundsInDIP(const gfx::Rect& bounds) override;
  gfx::Rect GetRestoredBoundsInDIP() const override;
  void SetWindowIcons(const gfx::ImageSkia& window_icon,
                      const gfx::ImageSkia& app_icon) override;
  void SizeConstraintsChanged() override;

  void PostBuffer();

 private:
  PlatformWindowDelegate* delegate_;
  QnxScreenWindowManager* manager_;
  screen_context_t ctx_;
  screen_window_t window_ = nullptr;
  gfx::Rect bounds_;
  bool visible_ = false;
  gfx::AcceleratedWidget widget_;
};

}  // namespace ui

#endif
