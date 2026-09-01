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

using QnxScreenExitCallback = void (*)();
void SetQnxScreenExitCallback(QnxScreenExitCallback cb);
QnxScreenExitCallback GetQnxScreenExitCallback();

// Fired on Navigator window-state changes: visible=true when the app is the
// foreground full-screen window, false when it is thumbnailed (multitask card)
// or fully covered. Used to mark the page hidden so rAF/timers/compositing
// throttle while backgrounded (CPU/battery back to the foreground app).
using QnxScreenVisibilityCallback = void (*)(bool visible);
void SetQnxScreenVisibilityCallback(QnxScreenVisibilityCallback cb);
QnxScreenVisibilityCallback GetQnxScreenVisibilityCallback();

// Fired when the Navigator delivers an invocation (NAVIGATOR_INVOKE_TARGET),
// e.g. the user tapped a pinned home-screen shortcut created with
// navigator_add_uri(). |uri| is the raw invocation URI (berrybrowser://...).
using QnxScreenInvokeUriCallback = void (*)(const char* uri);
void SetQnxScreenInvokeUriCallback(QnxScreenInvokeUriCallback cb);
QnxScreenInvokeUriCallback GetQnxScreenInvokeUriCallback();

// Fired when Navigator reports WINDOW_ACTIVE / WINDOW_STATE for a group.
// |group| may be null for the main card. |visible| is true when that card
// is fullscreen.
using QnxScreenActiveGroupCallback = void (*)(const char* group, bool visible);
void SetQnxScreenActiveGroupCallback(QnxScreenActiveGroupCallback cb);
QnxScreenActiveGroupCallback GetQnxScreenActiveGroupCallback();

}  // namespace ui

#endif
