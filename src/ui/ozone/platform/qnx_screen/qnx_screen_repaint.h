// Copyright 2025 SW7FT. All rights reserved.
// Request a repaint of the QNX Screen window.

#ifndef UI_OZONE_PLATFORM_QNX_SCREEN_QNX_SCREEN_REPAINT_H_
#define UI_OZONE_PLATFORM_QNX_SCREEN_QNX_SCREEN_REPAINT_H_

namespace ui {

void RequestQnxScreenRepaint();
bool ConsumeQnxScreenRepaintRequest();

}  // namespace ui

#endif
