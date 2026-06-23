// Copyright 2025 SW7FT. All rights reserved.

#include "ui/ozone/platform/qnx_screen/qnx_screen_repaint.h"

#include <atomic>

#include "base/atomicops.h"
#include "base/time/time.h"

namespace ui {

namespace {
base::subtle::Atomic32 g_repaint_requested = 0;
base::subtle::Atomic32 g_full_invalidate = 0;
base::subtle::Atomic32 g_finger_down = 0;
std::atomic<int64_t> g_last_touch_us{0};

constexpr int64_t kScrollMomentumUs = 250 * 1000;
}  // namespace

void RequestQnxScreenRepaint() {
  base::subtle::Release_Store(&g_repaint_requested, 1);
}

void RequestQnxScreenFullInvalidate() {
  base::subtle::Release_Store(&g_repaint_requested, 1);
  base::subtle::Release_Store(&g_full_invalidate, 1);
}

bool ConsumeQnxScreenRepaintRequest() {
  return base::subtle::Acquire_CompareAndSwap(&g_repaint_requested, 1, 0) == 1;
}

bool ConsumeQnxScreenFullInvalidateRequest() {
  return base::subtle::Acquire_CompareAndSwap(&g_full_invalidate, 1, 0) == 1;
}

void NotifyQnxScreenTouchGesture(bool finger_down, bool is_move) {
  g_last_touch_us.store(
      base::TimeTicks::Now().since_origin().InMicroseconds(),
      std::memory_order_relaxed);
  if (finger_down || is_move)
    base::subtle::Release_Store(&g_finger_down, 1);
  else
    base::subtle::Release_Store(&g_finger_down, 0);
}

bool QnxScreenScrollGestureActive() {
  if (base::subtle::Acquire_Load(&g_finger_down))
    return true;
  const int64_t now_us =
      base::TimeTicks::Now().since_origin().InMicroseconds();
  const int64_t last_us =
      g_last_touch_us.load(std::memory_order_relaxed);
  return last_us > 0 && (now_us - last_us) < kScrollMomentumUs;
}

}  // namespace ui
