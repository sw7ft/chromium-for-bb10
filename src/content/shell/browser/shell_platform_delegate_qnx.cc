// Copyright 2025 SW7FT. All rights reserved.
// QNX platform delegate for content_shell / BerryBrowserNative

#include "content/shell/browser/shell_platform_delegate.h"

#include "base/command_line.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/content_switches.h"
#include "content/shell/browser/shell.h"

#if !defined(HEADLESS_MODE_ONLY)
#include "content/shell/browser/berry_browser_chrome.h"
#endif

namespace content {

struct ShellPlatformDelegate::ShellData {
#if !defined(HEADLESS_MODE_ONLY)
  std::unique_ptr<BerryBrowserChrome> chrome;
#endif
};

struct ShellPlatformDelegate::PlatformData {};

ShellPlatformDelegate::ShellPlatformDelegate() = default;
ShellPlatformDelegate::~ShellPlatformDelegate() = default;

void ShellPlatformDelegate::Initialize(const gfx::Size& default_window_size) {}

void ShellPlatformDelegate::CreatePlatformWindow(
    Shell* shell,
    const gfx::Size& initial_size) {
  auto& data = shell_data_map_[shell];
#if !defined(HEADLESS_MODE_ONLY)
  bool headless = base::CommandLine::ForCurrentProcess()->HasSwitch(
      switches::kHeadless);
  if (!headless) {
    int w = initial_size.width() > 0 ? initial_size.width() : 1440;
    int h = initial_size.height() > 0 ? initial_size.height() : 1440;
    data.chrome = std::make_unique<BerryBrowserChrome>(w, h);
  }
#endif
}

gfx::NativeWindow ShellPlatformDelegate::GetNativeWindow(Shell* shell) {
  return {};
}

void ShellPlatformDelegate::CleanUp(Shell* shell) {
  shell_data_map_.erase(shell);
}

void ShellPlatformDelegate::SetContents(Shell* shell) {}

void ShellPlatformDelegate::ResizeWebContent(Shell* shell,
                                             const gfx::Size& content_size) {
  auto* rwhv = shell->web_contents()->GetRenderWidgetHostView();
  if (rwhv) {
#if !defined(HEADLESS_MODE_ONLY)
    auto it = shell_data_map_.find(shell);
    if (it != shell_data_map_.end() && it->second.chrome) {
      gfx::Rect wb = it->second.chrome->GetWebContentBounds();
      rwhv->SetSize(gfx::Size(wb.width(), wb.height()));
      return;
    }
#endif
    rwhv->SetSize(content_size);
  }
}

void ShellPlatformDelegate::EnableUIControl(Shell* shell,
                                            UIControl control,
                                            bool is_enabled) {
#if !defined(HEADLESS_MODE_ONLY)
  auto it = shell_data_map_.find(shell);
  if (it == shell_data_map_.end() || !it->second.chrome) return;
  switch (control) {
    case UIControl::BACK_BUTTON:
      it->second.chrome->SetCanGoBack(is_enabled);
      break;
    case UIControl::FORWARD_BUTTON:
      it->second.chrome->SetCanGoForward(is_enabled);
      break;
    case UIControl::STOP_BUTTON:
      break;
  }
#endif
}

void ShellPlatformDelegate::SetAddressBarURL(Shell* shell, const GURL& url) {
#if !defined(HEADLESS_MODE_ONLY)
  auto it = shell_data_map_.find(shell);
  if (it != shell_data_map_.end() && it->second.chrome)
    it->second.chrome->SetUrl(url.spec());
#endif
}

void ShellPlatformDelegate::SetIsLoading(Shell* shell, bool loading) {
#if !defined(HEADLESS_MODE_ONLY)
  auto it = shell_data_map_.find(shell);
  if (it != shell_data_map_.end() && it->second.chrome)
    it->second.chrome->SetLoading(loading);
#endif
}

void ShellPlatformDelegate::SetTitle(Shell* shell,
                                     const std::u16string& title) {}

void ShellPlatformDelegate::MainFrameCreated(Shell* shell) {}

bool ShellPlatformDelegate::DestroyShell(Shell* shell) {
  return false;
}

}  // namespace content
