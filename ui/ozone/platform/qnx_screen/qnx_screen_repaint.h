// Copyright 2025 SW7FT. All rights reserved.
// Request a repaint of the QNX Screen window.

#ifndef UI_OZONE_PLATFORM_QNX_SCREEN_QNX_SCREEN_REPAINT_H_
#define UI_OZONE_PLATFORM_QNX_SCREEN_QNX_SCREEN_REPAINT_H_

namespace ui {

// Height of the native toolbar overlay painted at y=0 (matches BerryBrowserChrome).
inline constexpr int kQnxScreenToolbarHeight = 56;

// Toolbar chrome changed; next present should include the toolbar in damage.
void RequestQnxScreenRepaint();

// Full-frame invalidate (e.g. after navigation). Also marks toolbar dirty.
void RequestQnxScreenFullInvalidate();

bool ConsumeQnxScreenRepaintRequest();
bool ConsumeQnxScreenFullInvalidateRequest();

// Touch scroll in progress (finger down or brief momentum after lift). Used to
// present full frames during gestures so partial Screen posts do not tear.
void NotifyQnxScreenTouchGesture(bool finger_down, bool is_move);
bool QnxScreenScrollGestureActive();

}  // namespace ui

#endif
