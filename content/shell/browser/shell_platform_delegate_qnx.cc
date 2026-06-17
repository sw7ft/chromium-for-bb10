// Copyright 2025 SW7FT. All rights reserved.
// QNX platform delegate for content_shell / BerryBrowserNative
//
// Unlike the previous stub, this creates a real aura WindowTreeHost +
// ui::Compositor (via ShellPlatformDataAura) so the browser actually
// composites the WebContents. The compositor's software output is presented
// to the device screen through the qnx_screen Ozone surface factory. Having a
// real browser-side Display is also what makes DevTools Page.captureScreenshot
// work (its CopyOutputRequest is satisfied during display aggregation).

#include "content/shell/browser/shell_platform_delegate.h"

#include "base/containers/contains.h"
#include "build/build_config.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/shell/browser/shell.h"
#include "content/shell/browser/shell_platform_data_aura.h"
#include "ui/aura/window.h"
#include "ui/aura/window_event_dispatcher.h"
#include "ui/aura/window_tree_host.h"

namespace content {

struct ShellPlatformDelegate::ShellData {
  gfx::NativeWindow window;
};

struct ShellPlatformDelegate::PlatformData {
  std::unique_ptr<ShellPlatformDataAura> aura;
};

ShellPlatformDelegate::ShellPlatformDelegate() = default;
ShellPlatformDelegate::~ShellPlatformDelegate() = default;

#if defined(USE_AURA) && !defined(SHELL_USE_TOOLKIT_VIEWS)
ShellPlatformDataAura* ShellPlatformDelegate::GetShellPlatformDataAura() {
  return platform_->aura.get();
}
#endif

void ShellPlatformDelegate::Initialize(const gfx::Size& default_window_size) {
  platform_ = std::make_unique<PlatformData>();
  platform_->aura =
      std::make_unique<ShellPlatformDataAura>(default_window_size);
}

void ShellPlatformDelegate::CreatePlatformWindow(
    Shell* shell,
    const gfx::Size& initial_size) {
  DCHECK(!base::Contains(shell_data_map_, shell));
  ShellData& shell_data = shell_data_map_[shell];

  platform_->aura->ResizeWindow(initial_size);

  shell_data.window = platform_->aura->host()->window();
}

gfx::NativeWindow ShellPlatformDelegate::GetNativeWindow(Shell* shell) {
  DCHECK(base::Contains(shell_data_map_, shell));
  ShellData& shell_data = shell_data_map_[shell];
  return shell_data.window;
}

void ShellPlatformDelegate::CleanUp(Shell* shell) {
  DCHECK(base::Contains(shell_data_map_, shell));
  shell_data_map_.erase(shell);
}

void ShellPlatformDelegate::SetContents(Shell* shell) {
  aura::Window* content = shell->web_contents()->GetNativeView();
  aura::Window* parent = platform_->aura->host()->window();
  if (!parent->Contains(content))
    parent->AddChild(content);

  content->Show();
}

void ShellPlatformDelegate::ResizeWebContent(Shell* shell,
                                             const gfx::Size& content_size) {
  auto* rwhv = shell->web_contents()->GetRenderWidgetHostView();
  if (rwhv)
    rwhv->SetSize(content_size);
}

void ShellPlatformDelegate::EnableUIControl(Shell* shell,
                                            UIControl control,
                                            bool is_enabled) {}

void ShellPlatformDelegate::SetAddressBarURL(Shell* shell, const GURL& url) {}

void ShellPlatformDelegate::SetIsLoading(Shell* shell, bool loading) {}

void ShellPlatformDelegate::SetTitle(Shell* shell,
                                     const std::u16string& title) {}

void ShellPlatformDelegate::MainFrameCreated(Shell* shell) {}

bool ShellPlatformDelegate::DestroyShell(Shell* shell) {
  return false;  // Shell destroys itself.
}

}  // namespace content
