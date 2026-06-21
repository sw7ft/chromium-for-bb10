// Copyright 2025 SW7FT. All rights reserved.
// Software surface factory for QNX Screen Ozone platform

#include "ui/ozone/platform/qnx_screen/qnx_screen_surface_factory.h"

#include <screen/screen.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "base/memory/ptr_util.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "ui/gfx/skia_util.h"
#include "ui/gfx/vsync_provider.h"
#include "ui/gl/gl_implementation.h"
#include "ui/ozone/platform/qnx_screen/qnx_screen_gl_ozone_egl.h"
#include "ui/ozone/platform/qnx_screen/qnx_screen_window.h"
#include "ui/ozone/platform/qnx_screen/qnx_screen_window_manager.h"
#include "ui/ozone/platform/qnx_screen/qnx_screen_overlay_callback.h"
#include "ui/ozone/public/surface_ozone_canvas.h"

namespace ui {

namespace {

class QnxScreenCanvas : public SurfaceOzoneCanvas {
 public:
  explicit QnxScreenCanvas(QnxScreenWindow* window)
      : window_(window) {
    ResizeCanvasInternal(gfx::Size(window->GetBoundsInPixels().width(),
                                   window->GetBoundsInPixels().height()));
  }

  ~QnxScreenCanvas() override = default;

  SkCanvas* GetCanvas() override {
    return surface_ ? surface_->getCanvas() : nullptr;
  }

  void ResizeCanvas(const gfx::Size& viewport_size, float scale) override {
    ResizeCanvasInternal(viewport_size);
  }

  void PresentCanvas(const gfx::Rect& damage) override {
    if (!window_ || !window_->screen_window())
      return;

    auto overlay_cb = GetQnxScreenOverlayPaintCallback();
    if (overlay_cb && surface_) {
      SkCanvas* canvas = surface_->getCanvas();
      if (canvas) {
        canvas->save();
        overlay_cb(canvas);
        canvas->restore();
      }
    }

    screen_buffer_t buf[2];
    int rc = screen_get_window_property_pv(window_->screen_window(),
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

    SkPixmap pixmap;
    if (surface_ && surface_->peekPixels(&pixmap)) {
      int h = pixmap.height();
      int copy_stride = std::min((int)pixmap.rowBytes(), stride);
      const uint8_t* src = static_cast<const uint8_t*>(pixmap.addr());
      uint8_t* dst = static_cast<uint8_t*>(ptr);
      for (int y = 0; y < h; y++) {
        memcpy(dst, src, copy_stride);
        src += pixmap.rowBytes();
        dst += stride;
      }

      // Debug-only: dump the composited software framebuffer so first-pixels
      // can be verified off-device. Gated on env so it has no production cost.
      // Format: "QFB1" magic, int32 width, height, rowbytes (LE), then raw
      // N32 (BGRA premul) pixels. Convert to PNG on the host.
      static const char* dump_path = getenv("QNX_FB_DUMP");
      static int dump_seq = 0;
      if (dump_path && dump_path[0]) {
        char namebuf[512];
        snprintf(namebuf, sizeof(namebuf), "%s.%03d", dump_path, dump_seq++);
        FILE* f = fopen(namebuf, "wb");
        if (f) {
          int32_t w = pixmap.width();
          int32_t hh = pixmap.height();
          int32_t rb = static_cast<int32_t>(pixmap.rowBytes());
          fwrite("QFB1", 1, 4, f);
          fwrite(&w, sizeof(w), 1, f);
          fwrite(&hh, sizeof(hh), 1, f);
          fwrite(&rb, sizeof(rb), 1, f);
          fwrite(pixmap.addr(), 1, static_cast<size_t>(rb) * hh, f);
          fclose(f);
        }
      }
    }

    int dirty[4] = {damage.x(), damage.y(),
                    damage.x() + damage.width(),
                    damage.y() + damage.height()};
    screen_post_window(window_->screen_window(), buf[0], 1, dirty, 0);
  }

  std::unique_ptr<gfx::VSyncProvider> CreateVSyncProvider() override {
    return nullptr;
  }

 private:
  void ResizeCanvasInternal(const gfx::Size& size) {
    SkImageInfo info = SkImageInfo::MakeN32Premul(size.width(), size.height());
    surface_ = SkSurfaces::Raster(info);
  }

  QnxScreenWindow* window_;
  sk_sp<SkSurface> surface_;
};

}  // namespace

QnxScreenSurfaceFactory::QnxScreenSurfaceFactory(
    QnxScreenWindowManager* window_manager)
    : window_manager_(window_manager),
      egl_ozone_(std::make_unique<QnxScreenGLOzoneEGL>(window_manager)) {}

QnxScreenSurfaceFactory::~QnxScreenSurfaceFactory() = default;

std::vector<gl::GLImplementationParts>
QnxScreenSurfaceFactory::GetAllowedGLImplementations() {
  // Native EGL/GLES2 (Adreno 330). Software is still used when --disable-gpu is
  // set; Chromium only routes to the GLOzone when GPU is enabled.
  fprintf(stderr, "QNX GL: GetAllowedGLImplementations -> EGLGLES2\n");
  return {gl::GLImplementationParts(gl::kGLImplementationEGLGLES2)};
}

GLOzone* QnxScreenSurfaceFactory::GetGLOzone(
    const gl::GLImplementationParts& implementation) {
  switch (implementation.gl) {
    case gl::kGLImplementationEGLGLES2:
      return egl_ozone_.get();
    default:
      return nullptr;
  }
}

std::unique_ptr<SurfaceOzoneCanvas>
QnxScreenSurfaceFactory::CreateCanvasForWidget(
    gfx::AcceleratedWidget widget) {
  QnxScreenWindow* window = window_manager_->GetWindow(widget);
  if (!window)
    return nullptr;
  return std::make_unique<QnxScreenCanvas>(window);
}

}  // namespace ui
