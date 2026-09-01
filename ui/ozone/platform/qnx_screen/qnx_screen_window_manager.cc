// Copyright 2025 SW7FT. All rights reserved.

#include "ui/ozone/platform/qnx_screen/qnx_screen_window_manager.h"

#include <cstring>

#include "ui/ozone/platform/qnx_screen/qnx_screen_window.h"

namespace ui {
namespace {
QnxScreenWindowManager* g_window_manager = nullptr;
}

QnxScreenWindowManager::QnxScreenWindowManager() {
  g_window_manager = this;
}
QnxScreenWindowManager::~QnxScreenWindowManager() {
  if (g_window_manager == this)
    g_window_manager = nullptr;
}

QnxScreenWindowManager* GetQnxScreenWindowManager() {
  return g_window_manager;
}

void QnxScreenWindowManager::AddWindow(gfx::AcceleratedWidget widget,
                                       QnxScreenWindow* window) {
  windows_[widget] = window;
  last_added_ = window;
}

void QnxScreenWindowManager::RemoveWindow(gfx::AcceleratedWidget widget) {
  auto it = windows_.find(widget);
  if (it != windows_.end() && last_added_ == it->second)
    last_added_ = nullptr;
  windows_.erase(widget);
  if (!last_added_ && !windows_.empty())
    last_added_ = windows_.begin()->second;
}

QnxScreenWindow* QnxScreenWindowManager::GetWindow(
    gfx::AcceleratedWidget widget) {
  auto it = windows_.find(widget);
  return it != windows_.end() ? it->second : nullptr;
}

QnxScreenWindow* QnxScreenWindowManager::GetFirstWindow() {
  return windows_.empty() ? nullptr : windows_.begin()->second;
}

QnxScreenWindow* QnxScreenWindowManager::GetLastAddedWindow() {
  return last_added_;
}

QnxScreenWindow* QnxScreenWindowManager::FindByGroup(const char* group) {
  if (!group || !group[0])
    return nullptr;
  for (auto& e : windows_) {
    if (e.second && e.second->group_name() &&
        strcmp(e.second->group_name(), group) == 0)
      return e.second;
  }
  return nullptr;
}

}  // namespace ui
