// Copyright 2025 SW7FT. All rights reserved.
// Callback for painting toolbar overlay on QNX Screen canvas.

#ifndef UI_OZONE_PLATFORM_QNX_SCREEN_QNX_SCREEN_OVERLAY_CALLBACK_H_
#define UI_OZONE_PLATFORM_QNX_SCREEN_QNX_SCREEN_OVERLAY_CALLBACK_H_

class SkCanvas;

namespace ui {

using QnxScreenOverlayPaintCallback = void (*)(SkCanvas* canvas);

void SetQnxScreenOverlayPaintCallback(QnxScreenOverlayPaintCallback cb);
QnxScreenOverlayPaintCallback GetQnxScreenOverlayPaintCallback();

// Rows the chrome overlay occupies, for the toolbar-only repaint path. The
// toolbar height scales with the render surface, so it cannot be a constant.
// 0 means there is no chrome at all.
void SetQnxScreenOverlayHeight(int height);
int GetQnxScreenOverlayHeight();

}  // namespace ui

#endif
