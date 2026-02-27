// Copyright 2025 SW7FT. All rights reserved.
// Callback for intercepting input events at the toolbar.

#ifndef UI_OZONE_PLATFORM_QNX_SCREEN_QNX_SCREEN_INPUT_CALLBACK_H_
#define UI_OZONE_PLATFORM_QNX_SCREEN_QNX_SCREEN_INPUT_CALLBACK_H_

namespace ui {

// Returns true if the event was consumed by the toolbar.
using QnxScreenTouchCallback = bool (*)(int type, int x, int y);
// type: 0=press, 1=move, 2=release

using QnxScreenKeyCallback = bool (*)(int key_sym, bool press);

void SetQnxScreenTouchCallback(QnxScreenTouchCallback cb);
QnxScreenTouchCallback GetQnxScreenTouchCallback();

void SetQnxScreenKeyCallback(QnxScreenKeyCallback cb);
QnxScreenKeyCallback GetQnxScreenKeyCallback();

}  // namespace ui

#endif
