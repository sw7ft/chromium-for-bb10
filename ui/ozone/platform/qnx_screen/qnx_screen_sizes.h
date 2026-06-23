// Copyright 2025 SW7FT. All rights reserved.
// Render vs panel size for QNX Screen (Passport defaults: 720 render, 1440 panel).

#ifndef UI_OZONE_PLATFORM_QNX_SCREEN_QNX_SCREEN_SIZES_H_
#define UI_OZONE_PLATFORM_QNX_SCREEN_QNX_SCREEN_SIZES_H_

#include <cstdlib>

namespace ui {

inline int QnxScreenEnvInt(const char* name, int default_val) {
  if (const char* e = getenv(name)) {
    int v = atoi(e);
    if (v > 0)
      return v;
  }
  return default_val;
}

inline void QnxScreenGetRenderSize(int* width, int* height) {
  *width = QnxScreenEnvInt("QNX_SCREEN_WIDTH", 720);
  *height = QnxScreenEnvInt("QNX_SCREEN_HEIGHT", 720);
}

inline void QnxScreenGetOutputSize(int* width, int* height) {
  *width = QnxScreenEnvInt("QNX_SCREEN_OUTPUT_WIDTH", 1440);
  *height = QnxScreenEnvInt("QNX_SCREEN_OUTPUT_HEIGHT", 1440);
}

inline void QnxScreenMapOutputToRender(int output_x,
                                      int output_y,
                                      int* render_x,
                                      int* render_y) {
  int rw = 720, rh = 720, ow = 1440, oh = 1440;
  QnxScreenGetRenderSize(&rw, &rh);
  QnxScreenGetOutputSize(&ow, &oh);
  *render_x = output_x * rw / ow;
  *render_y = output_y * rh / oh;
}

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_QNX_SCREEN_QNX_SCREEN_SIZES_H_
