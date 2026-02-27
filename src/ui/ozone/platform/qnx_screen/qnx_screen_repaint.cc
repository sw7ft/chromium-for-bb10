// Copyright 2025 SW7FT. All rights reserved.

#include "ui/ozone/platform/qnx_screen/qnx_screen_repaint.h"

#include "base/atomicops.h"

namespace ui {

namespace {
base::subtle::Atomic32 g_repaint_requested = 0;
}

void RequestQnxScreenRepaint() {
  base::subtle::Release_Store(&g_repaint_requested, 1);
}

bool ConsumeQnxScreenRepaintRequest() {
  return base::subtle::Acquire_CompareAndSwap(&g_repaint_requested, 1, 0) == 1;
}

}  // namespace ui
