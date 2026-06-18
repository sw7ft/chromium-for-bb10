// Copyright 2025 SW7FT. All rights reserved.
// QNX Screen window implementation for Ozone

#include "ui/ozone/platform/qnx_screen/qnx_screen_window.h"

#include <screen/screen.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>

#include "base/qnx_trace.h"
#include "ui/base/cursor/platform_cursor.h"
#include "ui/events/event.h"
#include "ui/events/platform/platform_event_source.h"
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

  // On the BB10 single-window display we fill the whole panel rather than honor
  // content_shell's 800x600 default. Default to the Passport's 1440x1440;
  // QNX_SCREEN_WIDTH/HEIGHT override for other panels/bring-up. Overriding
  // bounds_ here (before OnAcceleratedWidgetAvailable) makes Chromium's
  // compositor render at full size too, so the page isn't a small patch.
  int disp_w = 1440;
  int disp_h = 1440;
  if (const char* e = getenv("QNX_SCREEN_WIDTH")) {
    int v = atoi(e);
    if (v > 0)
      disp_w = v;
  }
  if (const char* e = getenv("QNX_SCREEN_HEIGHT")) {
    int v = atoi(e);
    if (v > 0)
      disp_h = v;
  }
  bounds_ = gfx::Rect(0, 0, disp_w, disp_h);

  int rc = screen_create_window(&window_, ctx_);
  if (rc != 0) {
    QNX_TRACE_MSG("QNX:OzWin: screen_create_window failed\n");
    return;
  }

  // On BB10 the Navigator only routes touch/keyboard input to windows that
  // belong to a window group it manages. A bare top-level window still gets
  // composited (the page draws), but taps go nowhere. Create a window group so
  // the Navigator recognizes this as the app's main window and forwards input.
  // (This mirrors what Qt's/SDL's QNX backends do for top-level windows.)
  snprintf(group_name_, sizeof(group_name_), "berryshell_%d", getpid());
  if (screen_create_window_group(window_, group_name_) != 0)
    QNX_TRACE_MSG("QNX:OzWin: screen_create_window_group failed\n");

  // Make sure the window participates in input hit-testing.
  int sensitivity = SCREEN_SENSITIVITY_ALWAYS;
  screen_set_window_property_iv(window_, SCREEN_PROPERTY_SENSITIVITY,
                                &sensitivity);

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

  // The Navigator composites app windows with an orientation transform; our
  // buffer is drawn upright, so allow correcting the rotation without a rebuild.
  // QNX_SCREEN_ROTATION = 0/90/180/270 (counter-clockwise degrees).
  int rotation = 0;
  if (const char* e = getenv("QNX_SCREEN_ROTATION")) {
    rotation = atoi(e);
  }
  screen_set_window_property_iv(window_, SCREEN_PROPERTY_ROTATION, &rotation);

  int zorder = 100;
  screen_set_window_property_iv(window_, SCREEN_PROPERTY_ZORDER, &zorder);

  int visible = 1;
  screen_set_window_property_iv(window_, SCREEN_PROPERTY_VISIBLE, &visible);
  // The window is created visible at the Screen level; reflect that in visible_
  // so CanDispatchEvent() lets input through even if Aura never calls Show().
  visible_ = true;

  rc = screen_create_window_buffers(window_, 2);
  if (rc != 0) {
    QNX_TRACE_MSG("QNX:OzWin: screen_create_window_buffers failed\n");
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
      QNX_TRACE_MSG("QNX:OzWin: Initial buffer posted (dark blue)\n");
    }
  }

  manager_->AddWindow(widget_, this);

  // Receive touch/keyboard events from QnxScreenEventSource and route them to
  // the Aura WindowTreeHost (our delegate), which feeds the gesture recognizer
  // and web content.
  if (PlatformEventSource::GetInstance())
    PlatformEventSource::GetInstance()->AddPlatformEventDispatcher(this);

  delegate_->OnAcceleratedWidgetAvailable(widget_);
}

QnxScreenWindow::~QnxScreenWindow() {
  if (PlatformEventSource::GetInstance())
    PlatformEventSource::GetInstance()->RemovePlatformEventDispatcher(this);
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
  // Single full-screen browser: content_shell asks to size the window to its
  // 800x600 default, which would shrink the on-screen window (SIZE/SOURCE_SIZE)
  // to a small patch of the panel while the compositor stays full-size. Ignore
  // shrink requests and keep the window pinned to the full display size, so the
  // page fills the screen. We still report the pinned bounds so web content
  // lays out at full size.
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

bool QnxScreenWindow::CanDispatchEvent(const PlatformEvent& event) {
  return visible_;
}

uint32_t QnxScreenWindow::DispatchEvent(const PlatformEvent& event) {
  static int s_dispatch_count = 0;
  if (s_dispatch_count++ < 8)
    QNX_TRACE_FMT("QNX:OzWin: dispatch->delegate #%d vis=%d\n",
                  s_dispatch_count, visible_ ? 1 : 0);
  delegate_->DispatchEvent(event);
  return POST_DISPATCH_STOP_PROPAGATION;
}

void QnxScreenWindow::PostBuffer() {
  if (!window_ || !visible_)
    return;
  int dirty[4] = {0, 0, bounds_.width(), bounds_.height()};
  screen_post_window(window_, nullptr, 0, dirty, 0);
}

}  // namespace ui
