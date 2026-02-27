// Copyright 2025 SW7FT. All rights reserved.
// QNX Screen window implementation for Ozone

#include "ui/ozone/platform/qnx_screen/qnx_screen_window.h"

#include <screen/screen.h>
#include <unistd.h>

#include "ui/base/cursor/platform_cursor.h"
#include "ui/ozone/platform/qnx_screen/qnx_screen_window_manager.h"

namespace ui {

namespace {
int g_next_widget_id = 1;
}

QnxScreenWindow::QnxScreenWindow(PlatformWindowDelegate* delegate,
                                 QnxScreenWindowManager* manager,
                                 screen_context_t ctx,
                                 const gfx::Rect& bounds)
    : delegate_(delegate),
      manager_(manager),
      ctx_(ctx),
      bounds_(bounds) {
  widget_ = static_cast<gfx::AcceleratedWidget>(g_next_widget_id++);

  int rc = screen_create_window(&window_, ctx_);
  if (rc != 0) {
    const char msg[] = "QNX:OzWin: screen_create_window failed\n";
    ::write(2, msg, sizeof(msg) - 1);
    return;
  }

  int format = SCREEN_FORMAT_RGBX8888;
  screen_set_window_property_iv(window_, SCREEN_PROPERTY_FORMAT, &format);

  int usage = SCREEN_USAGE_WRITE | SCREEN_USAGE_NATIVE;
  screen_set_window_property_iv(window_, SCREEN_PROPERTY_USAGE, &usage);

  int size[2] = {bounds_.width(), bounds_.height()};
  if (size[0] <= 0) size[0] = 1440;
  if (size[1] <= 0) size[1] = 1440;
  screen_set_window_property_iv(window_, SCREEN_PROPERTY_SIZE, size);
  screen_set_window_property_iv(window_, SCREEN_PROPERTY_SOURCE_SIZE, size);
  screen_set_window_property_iv(window_, SCREEN_PROPERTY_BUFFER_SIZE, size);

  int zorder = 100;
  screen_set_window_property_iv(window_, SCREEN_PROPERTY_ZORDER, &zorder);

  int visible = 1;
  screen_set_window_property_iv(window_, SCREEN_PROPERTY_VISIBLE, &visible);

  rc = screen_create_window_buffers(window_, 2);
  if (rc != 0) {
    const char msg[] = "QNX:OzWin: screen_create_window_buffers failed\n";
    ::write(2, msg, sizeof(msg) - 1);
  } else {
    // Fill initial buffer with a visible color to confirm window is showing
    screen_buffer_t buf[2];
    screen_get_window_property_pv(window_, SCREEN_PROPERTY_RENDER_BUFFERS,
                                  (void**)buf);
    void* ptr = nullptr;
    screen_get_buffer_property_pv(buf[0], SCREEN_PROPERTY_POINTER, &ptr);
    if (ptr) {
      int stride = 0;
      screen_get_buffer_property_iv(buf[0], SCREEN_PROPERTY_STRIDE, &stride);
      // Fill with dark blue (RGBX8888: 0xFF1a1a40)
      uint32_t* pixels = static_cast<uint32_t*>(ptr);
      for (int y = 0; y < size[1]; y++) {
        uint32_t* row = reinterpret_cast<uint32_t*>(
            static_cast<uint8_t*>(ptr) + y * stride);
        for (int x = 0; x < size[0]; x++)
          row[x] = 0xFF401a1a;  // BGRA dark blue
      }
      int dirty[4] = {0, 0, size[0], size[1]};
      screen_post_window(window_, buf[0], 1, dirty, 0);
      const char msg[] = "QNX:OzWin: Initial buffer posted (dark blue)\n";
      ::write(2, msg, sizeof(msg) - 1);
    }
  }

  manager_->AddWindow(widget_, this);
  delegate_->OnAcceleratedWidgetAvailable(widget_);
}

QnxScreenWindow::~QnxScreenWindow() {
  manager_->RemoveWindow(widget_);
  if (window_) {
    screen_destroy_window(window_);
    window_ = nullptr;
  }
}

gfx::Rect QnxScreenWindow::GetBoundsInPixels() const {
  return bounds_;
}

void QnxScreenWindow::SetBoundsInPixels(const gfx::Rect& bounds) {
  bounds_ = bounds;
  if (window_) {
    int size[2] = {bounds_.width(), bounds_.height()};
    screen_set_window_property_iv(window_, SCREEN_PROPERTY_SIZE, size);
    screen_set_window_property_iv(window_, SCREEN_PROPERTY_SOURCE_SIZE, size);
  }
  delegate_->OnBoundsChanged({/*origin_changed=*/true});
}

gfx::Rect QnxScreenWindow::GetBoundsInDIP() const {
  return bounds_;
}

void QnxScreenWindow::SetBoundsInDIP(const gfx::Rect& bounds) {
  SetBoundsInPixels(bounds);
}

void QnxScreenWindow::Show(bool inactive) {
  visible_ = true;
  if (window_) {
    int visible = 1;
    screen_set_window_property_iv(window_, SCREEN_PROPERTY_VISIBLE, &visible);
    int dirty[4] = {0, 0, bounds_.width(), bounds_.height()};
    screen_post_window(window_, nullptr, 0, dirty, 0);
  }
}

void QnxScreenWindow::Hide() {
  visible_ = false;
  if (window_) {
    int visible = 0;
    screen_set_window_property_iv(window_, SCREEN_PROPERTY_VISIBLE, &visible);
    screen_flush_context(ctx_, 0);
  }
}

void QnxScreenWindow::Close() {
  delegate_->OnClosed();
}

bool QnxScreenWindow::IsVisible() const {
  return visible_;
}

void QnxScreenWindow::PrepareForShutdown() {}
void QnxScreenWindow::SetTitle(const std::u16string& title) {}
void QnxScreenWindow::SetCapture() {}
void QnxScreenWindow::ReleaseCapture() {}
bool QnxScreenWindow::HasCapture() const { return false; }

void QnxScreenWindow::SetFullscreen(bool fullscreen,
                                    int64_t target_display_id) {}

void QnxScreenWindow::Maximize() {}
void QnxScreenWindow::Minimize() {}
void QnxScreenWindow::Restore() {}

PlatformWindowState QnxScreenWindow::GetPlatformWindowState() const {
  return PlatformWindowState::kNormal;
}

void QnxScreenWindow::Activate() {}
void QnxScreenWindow::Deactivate() {}
void QnxScreenWindow::SetUseNativeFrame(bool use_native_frame) {}
bool QnxScreenWindow::ShouldUseNativeFrame() const { return false; }
void QnxScreenWindow::SetCursor(scoped_refptr<PlatformCursor> cursor) {}
void QnxScreenWindow::MoveCursorTo(const gfx::Point& location) {}
void QnxScreenWindow::ConfineCursorToBounds(const gfx::Rect& bounds) {}
void QnxScreenWindow::SetRestoredBoundsInDIP(const gfx::Rect& bounds) {}

gfx::Rect QnxScreenWindow::GetRestoredBoundsInDIP() const {
  return bounds_;
}

void QnxScreenWindow::SetWindowIcons(const gfx::ImageSkia& window_icon,
                                     const gfx::ImageSkia& app_icon) {}

void QnxScreenWindow::SizeConstraintsChanged() {}

void QnxScreenWindow::PostBuffer() {
  if (!window_ || !visible_)
    return;
  int dirty[4] = {0, 0, bounds_.width(), bounds_.height()};
  screen_post_window(window_, nullptr, 0, dirty, 0);
}

}  // namespace ui
