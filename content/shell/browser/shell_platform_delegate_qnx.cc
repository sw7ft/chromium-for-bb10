// Copyright 2025 SW7FT. All rights reserved.
// QNX platform delegate for content_shell / BerryBrowserNative

#include "content/shell/browser/shell_platform_delegate.h"

#include <sys/keycodes.h>

#include "base/containers/contains.h"
#include "base/qnx_trace.h"
#include "base/strings/escape.h"
#include "base/synchronization/lock.h"
#include "build/build_config.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/shell/browser/berry_browser_chrome.h"
#include "content/shell/browser/shell.h"
#include "content/shell/browser/shell_platform_data_aura.h"
#include "ui/aura/window.h"
#include "ui/aura/window_event_dispatcher.h"
#include "ui/aura/window_tree_host.h"
#include "ui/ozone/platform/qnx_screen/qnx_screen_input_callback.h"
#include "ui/ozone/platform/qnx_screen/qnx_screen_overlay_callback.h"
#include "ui/ozone/platform/qnx_screen/qnx_screen_repaint.h"
#include "ui/ozone/platform/qnx_screen/qnx_screen_sizes.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "url/gurl.h"

namespace content {

namespace {

Shell* g_active_shell = nullptr;
std::unique_ptr<BerryBrowserChrome> g_chrome;
base::Lock g_chrome_lock;
bool g_url_bar_editing = false;
std::string g_last_chrome_url;
bool g_last_loading = false;
bool g_last_can_back = false;
bool g_last_can_forward = false;

void RepaintChrome() {
  ui::RequestQnxScreenRepaint();
}

void RepaintChromeFull() {
  ui::RequestQnxScreenFullInvalidate();
}

GURL GoogleSearchUrl(const std::string& query) {
  return GURL("https://www.google.com/search?q=" +
              base::EscapeQueryParamValue(query, false));
}

GURL ResolveUserUrl(const std::string& input) {
  std::string v = input;
  while (!v.empty() && (v.back() == ' ' || v.back() == '\t'))
    v.pop_back();
  size_t start = 0;
  while (start < v.size() && (v[start] == ' ' || v[start] == '\t'))
    ++start;
  if (start > 0)
    v = v.substr(start);
  if (v.empty())
    return GURL();

  if (v.find("://") != std::string::npos) {
    GURL g(v);
    if (g.is_valid() && g.host().find("google.com") != std::string::npos &&
        g.path().find("/search") != 0) {
      std::string q = g.path();
      if (!q.empty() && q[0] == '/')
        q = q.substr(1);
      if (q.empty() || q == "webhp")
        return GURL("https://www.google.com/");
      return GoogleSearchUrl(q);
    }
    return g;
  }

  // Paths like "/search" or "google.com/foo" are not bare hostnames — treat as
  // a search query instead of navigating to a likely-404 URL.
  if (v[0] == '/' || v.find('/') != std::string::npos)
    return GoogleSearchUrl(v);

  if (v.find(' ') == std::string::npos && v.find('.') != std::string::npos)
    return GURL("https://" + v);
  return GoogleSearchUrl(v);
}

void CommitUrlBarNavigation() {
  Shell* shell = nullptr;
  std::string raw;
  {
    base::AutoLock lock(g_chrome_lock);
    if (!g_active_shell || !g_chrome || !g_chrome->url_committed())
      return;
    shell = g_active_shell;
    raw = g_chrome->committed_url();
    g_chrome->ClearCommit();
    g_url_bar_editing = false;
  }
  GURL url = ResolveUserUrl(raw);
  if (!url.is_valid() && !raw.empty())
    url = GoogleSearchUrl(raw);
  QNX_NAV_LOG_FMT("BerryNav: commit raw=\"%s\" url=\"%s\" valid=%d\n",
                  raw.c_str(), url.spec().c_str(), url.is_valid() ? 1 : 0);
  if (url.is_valid()) {
    g_last_chrome_url = url.spec();
    shell->LoadURL(url);
    RepaintChromeFull();
  } else {
    RepaintChrome();
  }
}

void EnsureWebContentsFocus() {
  if (g_url_bar_editing)
    return;
  if (g_active_shell && g_active_shell->web_contents())
    g_active_shell->web_contents()->Focus();
}

void PaintChromeOverlay(SkCanvas* canvas) {
  base::AutoLock lock(g_chrome_lock);
  if (g_chrome)
    g_chrome->Paint(canvas);
}

bool QnxTouchCallback(int type, int x, int y) {
  if (type != 0)
    return false;

  Shell* shell = nullptr;
  BerryBrowserChrome::HitResult hit = BerryBrowserChrome::HitResult::kNone;
  bool loading = false;
  bool was_editing = false;
  {
    base::AutoLock lock(g_chrome_lock);
    if (!g_active_shell || !g_chrome)
      return false;
    shell = g_active_shell;
    hit = g_chrome->HitTest(x, y);
    if (hit == BerryBrowserChrome::HitResult::kNone) {
      was_editing = g_url_bar_editing;
      if (g_url_bar_editing) {
        g_url_bar_editing = false;
        g_chrome->CancelEditing();
      }
    } else {
      loading = g_chrome->loading();
    }
  }

  if (hit == BerryBrowserChrome::HitResult::kNone) {
    if (was_editing)
      RepaintChrome();
    EnsureWebContentsFocus();
    return false;
  }

  switch (hit) {
    case BerryBrowserChrome::HitResult::kBack:
      shell->GoBackOrForward(-1);
      RepaintChromeFull();
      break;
    case BerryBrowserChrome::HitResult::kForward:
      shell->GoBackOrForward(1);
      RepaintChromeFull();
      break;
    case BerryBrowserChrome::HitResult::kReload:
      if (loading)
        shell->Stop();
      else
        shell->Reload();
      break;
    case BerryBrowserChrome::HitResult::kUrlBar: {
      base::AutoLock lock(g_chrome_lock);
      g_url_bar_editing = true;
      if (g_chrome)
        g_chrome->StartEditing();
      QNX_NAV_LOG("BerryNav: url bar edit start\n");
      break;
    }
    default:
      break;
  }
  RepaintChrome();
  return true;
}

bool QnxKeyCallback(int sym, bool down) {
  if (!down)
    return false;

  {
    base::AutoLock lock(g_chrome_lock);
    if (g_url_bar_editing && g_chrome) {
      if (sym == KEYCODE_RETURN || sym == KEYCODE_KP_ENTER || sym == 0x0d ||
          sym == 0x0a) {
        QNX_NAV_LOG_FMT("BerryNav: enter key sym=0x%x editing=1\n", sym);
        g_chrome->OnEnter();
        RepaintChrome();
        CommitUrlBarNavigation();
        return true;
      }
      if (sym == KEYCODE_BACKSPACE) {
        g_chrome->OnBackspace();
        RepaintChrome();
        return true;
      }
      if (sym == KEYCODE_ESCAPE) {
        g_url_bar_editing = false;
        g_chrome->CancelEditing();
        RepaintChrome();
        EnsureWebContentsFocus();
        return true;
      }
      if (sym >= 0x20 && sym < 0xE000) {
        g_chrome->OnChar(static_cast<char>(sym));
        RepaintChrome();
        return true;
      }
      return true;
    }
  }

  if (sym == KEYCODE_RETURN || sym == KEYCODE_KP_ENTER || sym == 0x0d ||
      sym == 0x0a) {
    QNX_NAV_LOG_FMT("BerryNav: enter key sym=0x%x editing=0 page\n", sym);
  }
  EnsureWebContentsFocus();
  return false;
}

void QnxExitCallback() {
  Shell::Shutdown();
}

}  // namespace

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
  ui::SetQnxScreenOverlayPaintCallback(PaintChromeOverlay);
  ui::SetQnxScreenTouchCallback(QnxTouchCallback);
  ui::SetQnxScreenKeyCallback(QnxKeyCallback);
  ui::SetQnxScreenExitCallback(QnxExitCallback);
}

void ShellPlatformDelegate::CreatePlatformWindow(
    Shell* shell,
    const gfx::Size& initial_size) {
  DCHECK(!base::Contains(shell_data_map_, shell));
  ShellData& shell_data = shell_data_map_[shell];
  {
    base::AutoLock lock(g_chrome_lock);
    g_active_shell = shell;
    int rw = initial_size.width() > 0 ? initial_size.width() : 720;
    int rh = initial_size.height() > 0 ? initial_size.height() : 720;
    ui::QnxScreenGetRenderSize(&rw, &rh);
    g_chrome = std::make_unique<BerryBrowserChrome>(rw, rh);
    g_url_bar_editing = false;
  }

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
  if (g_active_shell == shell) {
    base::AutoLock lock(g_chrome_lock);
    g_active_shell = nullptr;
    g_chrome.reset();
    g_url_bar_editing = false;
  }
  shell_data_map_.erase(shell);
}

void ShellPlatformDelegate::SetContents(Shell* shell) {
  aura::Window* content = shell->web_contents()->GetNativeView();
  aura::Window* parent = platform_->aura->host()->window();
  if (!parent->Contains(content))
    parent->AddChild(content);

  content->Show();
  {
    base::AutoLock lock(g_chrome_lock);
    if (g_chrome) {
      auto* rwhv = shell->web_contents()->GetRenderWidgetHostView();
      if (rwhv)
        rwhv->SetSize(g_chrome->GetWebContentBounds().size());
    }
  }
  shell->web_contents()->Focus();
  RepaintChrome();
}

void ShellPlatformDelegate::ResizeWebContent(Shell* shell,
                                             const gfx::Size& content_size) {
  auto* rwhv = shell->web_contents()->GetRenderWidgetHostView();
  if (!rwhv)
    return;
  {
    base::AutoLock lock(g_chrome_lock);
    if (g_chrome)
      rwhv->SetSize(g_chrome->GetWebContentBounds().size());
    else
      rwhv->SetSize(content_size);
  }
}

void ShellPlatformDelegate::EnableUIControl(Shell* shell,
                                            UIControl control,
                                            bool is_enabled) {
  base::AutoLock lock(g_chrome_lock);
  if (!g_chrome)
    return;
  if (control == BACK_BUTTON) {
    if (is_enabled == g_last_can_back)
      return;
    g_last_can_back = is_enabled;
    g_chrome->SetCanGoBack(is_enabled);
  } else if (control == FORWARD_BUTTON) {
    if (is_enabled == g_last_can_forward)
      return;
    g_last_can_forward = is_enabled;
    g_chrome->SetCanGoForward(is_enabled);
  } else {
    return;
  }
  RepaintChrome();
}

void ShellPlatformDelegate::SetAddressBarURL(Shell* shell, const GURL& url) {
  base::AutoLock lock(g_chrome_lock);
  if (!g_chrome || g_url_bar_editing)
    return;
  const std::string spec = url.spec();
  if (spec == g_last_chrome_url)
    return;
  g_last_chrome_url = spec;
  g_chrome->SetUrl(spec);
  RepaintChrome();
}

void ShellPlatformDelegate::SetIsLoading(Shell* shell, bool loading) {
  base::AutoLock lock(g_chrome_lock);
  if (!g_chrome)
    return;
  if (loading == g_last_loading)
    return;
  g_last_loading = loading;
  g_chrome->SetLoading(loading);
  RepaintChrome();
  if (!loading)
    g_url_bar_editing = false;
}

void ShellPlatformDelegate::SetTitle(Shell* shell,
                                     const std::u16string& title) {}

void ShellPlatformDelegate::MainFrameCreated(Shell* shell) {
  shell->web_contents()->Focus();
}

bool ShellPlatformDelegate::DestroyShell(Shell* shell) {
  return false;
}

}  // namespace content
