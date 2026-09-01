// Copyright 2025 SW7FT. All rights reserved.

#include "ui/ozone/platform/qnx_screen/qnx_screen_input_callback.h"

namespace ui {

namespace {
QnxScreenTouchCallback g_touch_callback = nullptr;
QnxScreenKeyCallback g_key_callback = nullptr;
QnxScreenExitCallback g_exit_callback = nullptr;
QnxScreenVisibilityCallback g_visibility_callback = nullptr;
QnxScreenInvokeUriCallback g_invoke_uri_callback = nullptr;
QnxScreenActiveGroupCallback g_active_group_callback = nullptr;
}

void SetQnxScreenTouchCallback(QnxScreenTouchCallback cb) {
  g_touch_callback = cb;
}

QnxScreenTouchCallback GetQnxScreenTouchCallback() {
  return g_touch_callback;
}

void SetQnxScreenKeyCallback(QnxScreenKeyCallback cb) {
  g_key_callback = cb;
}

QnxScreenKeyCallback GetQnxScreenKeyCallback() {
  return g_key_callback;
}

void SetQnxScreenExitCallback(QnxScreenExitCallback cb) {
  g_exit_callback = cb;
}

QnxScreenExitCallback GetQnxScreenExitCallback() {
  return g_exit_callback;
}

void SetQnxScreenVisibilityCallback(QnxScreenVisibilityCallback cb) {
  g_visibility_callback = cb;
}

QnxScreenVisibilityCallback GetQnxScreenVisibilityCallback() {
  return g_visibility_callback;
}

void SetQnxScreenInvokeUriCallback(QnxScreenInvokeUriCallback cb) {
  g_invoke_uri_callback = cb;
}

QnxScreenInvokeUriCallback GetQnxScreenInvokeUriCallback() {
  return g_invoke_uri_callback;
}

void SetQnxScreenActiveGroupCallback(QnxScreenActiveGroupCallback cb) {
  g_active_group_callback = cb;
}

QnxScreenActiveGroupCallback GetQnxScreenActiveGroupCallback() {
  return g_active_group_callback;
}

}  // namespace ui
