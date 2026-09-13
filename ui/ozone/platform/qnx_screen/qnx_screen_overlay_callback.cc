// Copyright 2025 SW7FT. All rights reserved.

#include "ui/ozone/platform/qnx_screen/qnx_screen_overlay_callback.h"

namespace ui {

namespace {
QnxScreenOverlayPaintCallback g_overlay_callback = nullptr;
int g_overlay_height = 56;
}

void SetQnxScreenOverlayPaintCallback(QnxScreenOverlayPaintCallback cb) {
  g_overlay_callback = cb;
}

QnxScreenOverlayPaintCallback GetQnxScreenOverlayPaintCallback() {
  return g_overlay_callback;
}

void SetQnxScreenOverlayHeight(int height) {
  g_overlay_height = height;
}

int GetQnxScreenOverlayHeight() {
  return g_overlay_height;
}

}  // namespace ui
