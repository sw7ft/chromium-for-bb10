// Copyright 2025 SW7FT. All rights reserved.

#include "ui/ozone/platform/qnx_screen/qnx_screen_window_manager.h"

namespace ui {

QnxScreenWindowManager::QnxScreenWindowManager() = default;
QnxScreenWindowManager::~QnxScreenWindowManager() = default;

void QnxScreenWindowManager::AddWindow(gfx::AcceleratedWidget widget,
                                       QnxScreenWindow* window) {
  windows_[widget] = window;
}

void QnxScreenWindowManager::RemoveWindow(gfx::AcceleratedWidget widget) {
  windows_.erase(widget);
}

QnxScreenWindow* QnxScreenWindowManager::GetWindow(
    gfx::AcceleratedWidget widget) {
  auto it = windows_.find(widget);
  return it != windows_.end() ? it->second : nullptr;
}

}  // namespace ui
