// Copyright 2025 SW7FT. All rights reserved.
// Shared VSync plumbing for the QNX Screen Ozone platform.
//
// Both present paths feed real present times into Viz through this provider so
// the begin-frame source paces to the panel instead of free-running:
//   - Software: QnxScreenCanvas::PresentCanvas after screen_post_window().
//   - GPU/EGL:  QnxNativeViewGLSurfaceEGL::SwapBuffers after eglSwapBuffers().
//
// Without a provider the GPU path leaves Viz with no vsync signal at all (BB10
// lacks EGL_CHROMIUM_sync_control), which produces multi-second QNX:BF
// begin-frame gaps. This header lets the GL surface reuse the exact mechanism
// the software canvas already uses.

#ifndef UI_OZONE_PLATFORM_QNX_SCREEN_QNX_SCREEN_VSYNC_H_
#define UI_OZONE_PLATFORM_QNX_SCREEN_QNX_SCREEN_VSYNC_H_

#include <screen/screen.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/qnx_trace.h"
#include "base/synchronization/lock.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "ui/gfx/vsync_provider.h"

namespace ui {

// Query the physical display's refresh interval for the given window. Without
// this, Viz falls back to a guessed 60Hz begin-frame timer that is never aligned
// to the panel, so frames are scheduled with up to a full interval of extra
// latency. Falls back to 60Hz if the mode can't be read or looks implausible.
inline base::TimeDelta QnxQueryDisplayInterval(screen_window_t win) {
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
// raster/composite + full-frame present cannot sustain 60fps on heavy/animated
// pages, so Viz keeps scheduling ~60 begin-frames/sec (which also drive the
// page's requestAnimationFrame and CSS animations) whose work mostly gets
// dropped -- wasted CPU. Pacing begin-frames at a lower rate throttles the
// page's animation work and the compositor together, while idle pages still go
// fully idle (the display-scheduler keep-alive handles that). Returned as the
// *minimum* begin-frame interval. Tunable via QNX_MAX_FPS (default 60 == no cap
// on a 60Hz panel).
inline base::TimeDelta QnxApplyMaxFpsCap(base::TimeDelta panel_interval) {
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
// post/swap time becomes the timebase) and read by the VSyncProvider that Viz
// polls.
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

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_QNX_SCREEN_QNX_SCREEN_VSYNC_H_
