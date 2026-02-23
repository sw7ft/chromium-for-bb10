// Copyright 2025 SW7FT. All rights reserved.
// Callback for painting toolbar overlay on QNX Screen canvas.

#ifndef UI_OZONE_PLATFORM_QNX_SCREEN_QNX_SCREEN_OVERLAY_CALLBACK_H_
#define UI_OZONE_PLATFORM_QNX_SCREEN_QNX_SCREEN_OVERLAY_CALLBACK_H_

class SkCanvas;

namespace ui {

using QnxScreenOverlayPaintCallback = void (*)(SkCanvas* canvas);

void SetQnxScreenOverlayPaintCallback(QnxScreenOverlayPaintCallback cb);
QnxScreenOverlayPaintCallback GetQnxScreenOverlayPaintCallback();

}  // namespace ui

#endif
