// Copyright 2025 SW7FT. All rights reserved.
// Window manager for the QNX Screen Ozone platform

#ifndef UI_OZONE_PLATFORM_QNX_SCREEN_QNX_SCREEN_WINDOW_MANAGER_H_
#define UI_OZONE_PLATFORM_QNX_SCREEN_QNX_SCREEN_WINDOW_MANAGER_H_

#include <map>

#include "ui/gfx/native_widget_types.h"

namespace ui {

class QnxScreenWindow;

class QnxScreenWindowManager {
 public:
  QnxScreenWindowManager();
  ~QnxScreenWindowManager();

  void AddWindow(gfx::AcceleratedWidget widget, QnxScreenWindow* window);
  void RemoveWindow(gfx::AcceleratedWidget widget);
  QnxScreenWindow* GetWindow(gfx::AcceleratedWidget widget);
  QnxScreenWindow* GetFirstWindow();

 private:
  std::map<gfx::AcceleratedWidget, QnxScreenWindow*> windows_;
};

}  // namespace ui

#endif
