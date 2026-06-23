// Copyright 2025 SW7FT. All rights reserved.
// Software surface factory for QNX Screen Ozone platform

#include "ui/ozone/platform/qnx_screen/qnx_screen_surface_factory.h"

#include <screen/screen.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "base/memory/ptr_util.h"
#include "base/memory/scoped_refptr.h"
#include "base/qnx_pump_activity.h"
#include "base/qnx_trace.h"
#include "base/synchronization/lock.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/skia_util.h"
#include "ui/gfx/vsync_provider.h"
#include "ui/gl/gl_implementation.h"
#include "ui/ozone/platform/qnx_screen/qnx_screen_gl_ozone_egl.h"
#include "ui/ozone/platform/qnx_screen/qnx_screen_window.h"
#include "ui/ozone/platform/qnx_screen/qnx_screen_window_manager.h"
#include "ui/ozone/platform/qnx_screen/qnx_screen_overlay_callback.h"
#include "ui/ozone/platform/qnx_screen/qnx_screen_repaint.h"
#include "ui/ozone/public/surface_ozone_canvas.h"

namespace ui {

namespace {

// Query the physical display's refresh interval for the given window. Without
// this, Viz falls back to a guessed 60Hz begin-frame timer that is never aligned
// to the panel, so frames are scheduled with up to a full interval of extra
// latency. Falls back to 60Hz if the mode can't be read or looks implausible.
base::TimeDelta QueryDisplayInterval(screen_window_t win) {
  const base::TimeDelta kFallback = base::Hertz(60);
  if (!win)
    return kFallback;
  screen_display_t display = nullptr;
  if (screen_get_window_property_pv(win, SCREEN_PROPERTY_DISPLAY,
                                    reinterpret_cast<void**>(&display)) != 0 ||
      !display) {
    return kFallback;
  }
  screen_display_mode_t mode;
  if (screen_get_display_property_cv(display, SCREEN_PROPERTY_MODE,
                                     sizeof(mode),
                                     reinterpret_cast<char*>(&mode)) != 0) {
    return kFallback;
  }
  // Clamp to a sane panel range; BB10 Passport is 60Hz.
  if (mode.refresh < 30 || mode.refresh > 120) {
#if BUILDFLAG(IS_QNX)
    if (base::QnxFpsLogEnabled()) {
      char line[96];
      int n = snprintf(line, sizeof(line),
                       "QNX:VSYNC refresh=%d out of range, using 60Hz fallback\n",
                       mode.refresh);
      if (n > 0)
        ::write(2, line, n);
    }
#endif
    return kFallback;
  }
#if BUILDFLAG(IS_QNX)
  if (base::QnxFpsLogEnabled()) {
    char line[64];
    int n = snprintf(line, sizeof(line), "QNX:VSYNC refresh=%dHz\n",
                     mode.refresh);
    if (n > 0)
      ::write(2, line, n);
  }
#endif
  return base::Hertz(mode.refresh);
}

// Cap the begin-frame rate on this SoC. The panel is 60Hz, but full-screen
// software raster + full-frame present cannot sustain 60fps on heavy/animated
// pages, so Viz keeps scheduling ~60 begin-frames/sec (which also drive the
// page's requestAnimationFrame and CSS animations) whose work mostly gets
// dropped -- wasted CPU at ~99% on a single core. Pacing begin-frames at a lower
// rate throttles the page's animation work and the compositor together, roughly
// halving CPU on animated pages, while idle pages still go fully idle (the
// display-scheduler keep-alive handles that). Returned as the *minimum*
// begin-frame interval. Tunable via QNX_MAX_FPS (default 30); set 60 to
// effectively disable the cap on a 60Hz panel.
base::TimeDelta ApplyMaxFpsCap(base::TimeDelta panel_interval) {
  static const int kMaxFps = []() {
    const char* e = getenv("QNX_MAX_FPS");
    const int v = e ? atoi(e) : 60;  // default 60 == no cap on a 60Hz panel
    return (v >= 5 && v <= 120) ? v : 60;
  }();
  const base::TimeDelta kMinInterval = base::Hertz(kMaxFps);
#if BUILDFLAG(IS_QNX)
  if (base::QnxFpsLogEnabled()) {
    char line[64];
    int n = snprintf(line, sizeof(line), "QNX:MAXFPS cap=%dfps\n", kMaxFps);
    if (n > 0)
      ::write(2, line, n);
  }
#endif
  // Larger interval == lower fps, so the cap is a floor on the interval.
  return std::max(panel_interval, kMinInterval);
}

// Shared, thread-safe vsync parameters written by the present path (the actual
// post time becomes the timebase) and read by the VSyncProvider that Viz polls.
class QnxVSyncState : public base::RefCountedThreadSafe<QnxVSyncState> {
 public:
  explicit QnxVSyncState(base::TimeDelta interval)
      : interval_(interval), timebase_(base::TimeTicks::Now()) {}

  void OnPresent(base::TimeTicks present_time) {
    base::AutoLock lock(lock_);
    timebase_ = present_time;
  }

  void Get(base::TimeTicks* timebase, base::TimeDelta* interval) {
    base::AutoLock lock(lock_);
    *timebase = timebase_;
    *interval = interval_;
  }

 private:
  friend class base::RefCountedThreadSafe<QnxVSyncState>;
  ~QnxVSyncState() = default;

  base::Lock lock_;
  base::TimeDelta interval_;
  base::TimeTicks timebase_;
};

// Reports vsync timing to Viz so the begin-frame source paces to the real panel
// refresh and aligns its phase to the last present instead of free-running.
class QnxScreenVSyncProvider : public gfx::VSyncProvider {
 public:
  explicit QnxScreenVSyncProvider(scoped_refptr<QnxVSyncState> state)
      : state_(std::move(state)) {}
  ~QnxScreenVSyncProvider() override = default;

  void GetVSyncParameters(UpdateVSyncCallback callback) override {
    base::TimeTicks timebase;
    base::TimeDelta interval;
    state_->Get(&timebase, &interval);
    std::move(callback).Run(timebase, interval);
  }

  bool GetVSyncParametersIfAvailable(base::TimeTicks* timebase,
                                     base::TimeDelta* interval) override {
    state_->Get(timebase, interval);
    return true;
  }

  bool SupportGetVSyncParametersIfAvailable() const override { return true; }
  bool IsHWClock() const override { return false; }

 private:
  scoped_refptr<QnxVSyncState> state_;
};

class QnxScreenCanvas : public SurfaceOzoneCanvas {
 public:
  explicit QnxScreenCanvas(QnxScreenWindow* window)
      : window_(window),
        vsync_state_(base::MakeRefCounted<QnxVSyncState>(ApplyMaxFpsCap(
            QueryDisplayInterval(window ? window->screen_window() : nullptr)))) {
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
    RequestQnxScreenFullInvalidate();
    base::MarkQnxProcessPumpActive();
    const base::TimeTicks t_enter = base::TimeTicks::Now();

    auto overlay_cb = GetQnxScreenOverlayPaintCallback();
    if (overlay_cb && surface_) {
      SkCanvas* canvas = surface_->getCanvas();
      if (canvas) {
        canvas->save();
        overlay_cb(canvas);
        canvas->restore();
      }
    }

    (void)ConsumeQnxScreenFullInvalidateRequest();
    (void)ConsumeQnxScreenRepaintRequest();

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
    gfx::Rect post_rect;
    if (surface_ && surface_->peekPixels(&pixmap)) {
      const gfx::Rect bounds(pixmap.width(), pixmap.height());
      // Software present: always copy/post the full framebuffer so toolbar and
      // page update atomically (partial Screen posts tear on BB10).
      const gfx::Rect copy_rect = bounds;
      post_rect = bounds;

      if (!copy_rect.IsEmpty()) {
        const int bytes_per_pixel = 4;
        int buf_size[2] = {0, 0};
        screen_get_buffer_property_iv(buf[0], SCREEN_PROPERTY_BUFFER_SIZE,
                                      buf_size);
        const int copy_h =
            buf_size[1] > 0
                ? std::min(copy_rect.height(), buf_size[1] - copy_rect.y())
                : copy_rect.height();
        const int copy_w =
            buf_size[0] > 0
                ? std::min(copy_rect.width(), buf_size[0] - copy_rect.x())
                : copy_rect.width();
        const int row_bytes =
            std::min(copy_w * bytes_per_pixel, stride);
        const uint8_t* base_src =
            static_cast<const uint8_t*>(pixmap.addr());
        uint8_t* base_dst = static_cast<uint8_t*>(ptr);
        for (int y = copy_rect.y(); y < copy_rect.y() + copy_h; ++y) {
          const uint8_t* src_row =
              base_src + y * pixmap.rowBytes() +
              copy_rect.x() * bytes_per_pixel;
          uint8_t* dst_row =
              base_dst + y * stride + copy_rect.x() * bytes_per_pixel;
          memcpy(dst_row, src_row, row_bytes);
        }
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

    if (post_rect.IsEmpty())
      post_rect = damage;

    int dirty[4] = {post_rect.x(), post_rect.y(), post_rect.right(),
                    post_rect.bottom()};
    base::TimeTicks t_before_post = base::TimeTicks::Now();
    screen_post_window(window_->screen_window(), buf[0], 1, dirty, 0);

    // Feed the actual present time back as the vsync timebase so Viz aligns its
    // begin-frame phase to real presents (post is async, so this is the submit
    // time, not the hardware vblank; close enough to fix the free-running phase).
    base::TimeTicks now = base::TimeTicks::Now();
    vsync_state_->OnPresent(now);

    if (base::QnxFpsLogEnabled()) {
      // Break the per-frame budget into three parts so we can see *where* the
      // time goes when fps is low on an otherwise-idle CPU:
      //   gap_ms  = time from the previous present's exit to this present's
      //             entry == time spent upstream (begin-frame wait + raster +
      //             memcpy before post). High gap => throttled/blocked upstream.
      //   post_ms = time inside screen_post_window() == display-compositor
      //             stall. High post => present is blocking on the Screen svc.
      static base::TimeTicks s_window_start;
      static base::TimeTicks s_last_exit;
      static int s_frames = 0;
      static int s_max_damage_h = 0;
      static double s_sum_gap_ms = 0;
      static double s_sum_post_ms = 0;
      static double s_max_post_ms = 0;
      static double s_max_gap_ms = 0;
      if (s_window_start.is_null())
        s_window_start = t_enter;
      double post_ms = (now - t_before_post).InMicrosecondsF() / 1000.0;
      double gap_ms = s_last_exit.is_null()
                          ? 0.0
                          : (t_enter - s_last_exit).InMicrosecondsF() / 1000.0;
      s_frames++;
      s_sum_post_ms += post_ms;
      s_sum_gap_ms += gap_ms;
      if (post_ms > s_max_post_ms)
        s_max_post_ms = post_ms;
      if (gap_ms > s_max_gap_ms)
        s_max_gap_ms = gap_ms;
      if (damage.height() > s_max_damage_h)
        s_max_damage_h = damage.height();
      base::TimeDelta elapsed = now - s_window_start;
      if (elapsed >= base::Seconds(1)) {
        char line[224];
        int n = snprintf(
            line, sizeof(line),
            "QNX:FPS present=%d in %dms (maxDamageH=%d) "
            "post avg=%.1f max=%.1fms | gap avg=%.1f max=%.1fms\n",
            s_frames, (int)elapsed.InMilliseconds(), s_max_damage_h,
            s_sum_post_ms / s_frames, s_max_post_ms, s_sum_gap_ms / s_frames,
            s_max_gap_ms);
        if (n > 0)
          ::write(2, line, n);
        s_window_start = now;
        s_frames = 0;
        s_max_damage_h = 0;
        s_sum_gap_ms = 0;
        s_sum_post_ms = 0;
        s_max_post_ms = 0;
        s_max_gap_ms = 0;
      }
      s_last_exit = base::TimeTicks::Now();
    }
  }

  std::unique_ptr<gfx::VSyncProvider> CreateVSyncProvider() override {
    return std::make_unique<QnxScreenVSyncProvider>(vsync_state_);
  }

  int MaxFramesPending() const override { return 3; }

 private:
  void ResizeCanvasInternal(const gfx::Size& size) {
    SkImageInfo info = SkImageInfo::MakeN32Premul(size.width(), size.height());
    surface_ = SkSurfaces::Raster(info);
  }

  QnxScreenWindow* window_;
  scoped_refptr<QnxVSyncState> vsync_state_;
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
