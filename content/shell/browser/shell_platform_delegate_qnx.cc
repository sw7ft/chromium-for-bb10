// Copyright 2025 SW7FT. All rights reserved.
// QNX platform delegate for content_shell / BerryBrowserNative

#include "content/shell/browser/shell_platform_delegate.h"

#include <bps/navigator.h>
#include <sys/keycodes.h>
#include <unistd.h>

#include <cstdlib>
#include <map>

#include "content/public/browser/browser_context.h"

#include "base/containers/contains.h"
#include "base/functional/bind.h"
#include "base/qnx_hard_watchdog.h"
#include "base/qnx_trace.h"
#include "base/strings/escape.h"
#include "base/strings/string_split.h"
#include "base/synchronization/lock.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/visibility.h"
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
#include "ui/ozone/platform/qnx_screen/qnx_screen_window.h"
#include "ui/ozone/platform/qnx_screen/qnx_screen_window_manager.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "url/gurl.h"

extern "C" int __llvm_profile_write_file(void);

namespace content {

namespace {

Shell* g_active_shell = nullptr;
std::unique_ptr<BerryBrowserChrome> g_chrome;
std::map<std::string, Shell*> g_group_to_shell;
base::Lock g_chrome_lock;
bool g_url_bar_editing = false;
std::string g_last_chrome_url;
bool g_last_loading = false;
bool g_last_can_back = false;
bool g_last_can_forward = false;
base::TimeTicks g_last_chrome_url_repaint;

// "App mode": when BERRY_APP_MODE=1 the engine-drawn URL toolbar
// (BerryBrowserChrome) is never created, so single-site web apps (e.g. the
// WhatsApp .bar) render full-screen with no browser chrome -- the page gets the
// whole window. All g_chrome accesses below are null-guarded, so a null toolbar
// simply means no overlay paint, no toolbar hit-testing, and full-window web
// content sizing (see CreatePlatformWindow / SetContents).
bool BerryAppModeNoChrome() {
  static const bool no_chrome = [] {
    const char* e = getenv("BERRY_APP_MODE");
    return e && e[0] == '1';
  }();
  return no_chrome;
}

void RepaintChrome() {
  ui::RequestQnxScreenRepaint();
}

void RepaintChromeFull() {
  ui::RequestQnxScreenFullInvalidate();
}

GURL DecodeBerryInvokeUri(const char* raw, bool* new_window) {
  if (new_window)
    *new_window = false;
  if (!raw || !raw[0])
    return GURL();
  GURL u(raw);
  if (u.SchemeIsHTTPOrHTTPS() || u.SchemeIsFile())
    return u;
  if (u.scheme() == "berrybrowser") {
    std::string dest;
    base::StringPairs pairs;
    base::SplitStringIntoKeyValuePairs(u.query(), '=', '&', &pairs);
    for (const auto& p : pairs) {
      if (p.first == "u")
        dest = base::UnescapeBinaryURLComponent(p.second);
      else if (p.first == "win" && p.second == "1" && new_window)
        *new_window = true;
    }
    if (!dest.empty()) {
      GURL g(dest);
      if (g.is_valid())
        return g;
    }
  }
  return u;
}

bool BerryUrlLooksIdle(const GURL& u) {
  if (!u.is_valid() || u.IsAboutBlank())
    return true;
  if (u.host() == "berry.home" || u.host() == "berry.settings" ||
      u.host() == "berry.pin" || u.host() == "berry.set" ||
      u.host() == "berry.share")
    return true;
  if (u.SchemeIsFile()) {
    const std::string& p = u.path();
    return p.find("home.html") != std::string::npos ||
           p.find(".berry-") != std::string::npos;
  }
  return false;
}

bool BerryShellHoldsPlaceholder(Shell* shell) {
  if (!shell || !shell->web_contents())
    return true;
  return BerryUrlLooksIdle(shell->web_contents()->GetLastCommittedURL()) &&
         BerryUrlLooksIdle(shell->web_contents()->GetVisibleURL());
}

void BerryShowShell(Shell* target) {
  if (!target)
    return;
  int n = 0;
  for (Shell* s : Shell::windows()) {
    ++n;
    WebContents* wc = s->web_contents();
    if (!wc)
      continue;
    aura::Window* view = wc->GetNativeView();
    if (s == target) {
      if (view)
        view->Show();
      wc->UpdateWebContentsVisibility(Visibility::VISIBLE);
      wc->Focus();
    } else {
      if (view)
        view->Hide();
      wc->UpdateWebContentsVisibility(Visibility::HIDDEN);
    }
  }
  {
    base::AutoLock lock(g_chrome_lock);
    g_active_shell = target;
    if (g_chrome) {
      g_chrome->SetWindowCount(n > 0 ? n : 1);
      if (target->web_contents())
        g_chrome->SetUrl(target->web_contents()->GetVisibleURL().spec());
    }
  }
  QNX_NAV_LOG_FMT("BerryNav: show window %d/%d url=\"%s\"\n", n,
                  static_cast<int>(Shell::windows().size()),
                  target->web_contents()
                      ? target->web_contents()->GetVisibleURL().spec().c_str()
                      : "");
  RepaintChromeFull();
}

void BerryCycleWindow() {
  const std::vector<Shell*>& wins = Shell::windows();
  if (wins.size() < 2)
    return;
  size_t i = 0;
  for (; i < wins.size(); ++i) {
    if (wins[i] == g_active_shell)
      break;
  }
  if (i >= wins.size())
    i = 0;
  else
    i = (i + 1) % wins.size();
  BerryShowShell(wins[i]);
}

void BerryActivateShell(Shell* shell) {
  BerryShowShell(shell);
}

void QnxActiveGroupCallback(const char* group, bool visible) {
  if (!visible || !group || !group[0])
    return;
  auto it = g_group_to_shell.find(group);
  if (it == g_group_to_shell.end())
    return;
  QNX_NAV_LOG_FMT("BerryNav: activate group=\"%s\"\n", group);
  BerryActivateShell(it->second);
}

void QnxInvokeUriCallback(const char* uri) {
  if (!uri || !uri[0])
    return;
  std::string raw(uri);
  GetUIThreadTaskRunner({})->PostTask(
      FROM_HERE, base::BindOnce(
                     [](std::string raw) {
                       bool want_new = false;
                       const GURL dest =
                           DecodeBerryInvokeUri(raw.c_str(), &want_new);
                       if (!dest.is_valid()) {
                         QNX_NAV_LOG_FMT(
                             "BerryNav: invoke drop raw=\"%s\"\n",
                             raw.c_str());
                         return;
                       }
                       // Home-screen pins (berrybrowser://…&win=1, or any
                       // berrybrowser:// open) must NOT steal the existing
                       // card — even if that card is still on home.html.
                       // Only reuse a window already showing this exact URL.
                       const bool force_new =
                           want_new ||
                           raw.find("berrybrowser://") == 0;
                       Shell* reuse = nullptr;
                       for (Shell* s : Shell::windows()) {
                         if (!s->web_contents())
                           continue;
                         const GURL cur =
                             s->web_contents()->GetLastCommittedURL();
                         if (cur.is_valid() &&
                             cur.EqualsIgnoringRef(dest)) {
                           reuse = s;
                           break;
                         }
                       }
                       if (!force_new && !reuse) {
                         for (Shell* s : Shell::windows()) {
                           if (BerryShellHoldsPlaceholder(s)) {
                             reuse = s;
                             break;
                           }
                         }
                       }
                       QNX_NAV_LOG_FMT(
                           "BerryNav: invoke dest=\"%s\" new=%d reuse=%d "
                           "windows=%zu\n",
                           dest.spec().c_str(), force_new ? 1 : 0,
                           reuse ? 1 : 0, Shell::windows().size());
                       if (reuse) {
                         BerryShowShell(reuse);
                         return;
                       }
                       if (Shell::windows().empty())
                         return;
                       BrowserContext* ctx = Shell::windows()
                                                 .front()
                                                 ->web_contents()
                                                 ->GetBrowserContext();
                       Shell::CreateNewWindow(ctx, dest, nullptr, gfx::Size());
                     },
                     std::move(raw)));
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
    {
      base::AutoLock lock(g_chrome_lock);
      g_last_chrome_url = url.spec();
    }
    shell->LoadURL(url);
    RepaintChromeFull();
  } else {
    RepaintChrome();
  }
}

void EnsureWebContentsFocus() {
  Shell* shell = nullptr;
  {
    base::AutoLock lock(g_chrome_lock);
    if (g_url_bar_editing)
      return;
    shell = g_active_shell;
  }
  if (shell && shell->web_contents())
    shell->web_contents()->Focus();
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
    case BerryBrowserChrome::HitResult::kShare: {
      std::string share_url;
      {
        base::AutoLock lock(g_chrome_lock);
        if (g_chrome)
          share_url = g_chrome->url();
      }
      if (share_url.empty() && shell->web_contents())
        share_url = shell->web_contents()->GetVisibleURL().spec();
      BerryNavigatorShare(share_url);
      break;
    }
    case BerryBrowserChrome::HitResult::kWindows:
      BerryCycleWindow();
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

void QnxVisibilityCallback(bool visible) {
  // Drive the page's visibility from the Navigator window state. Hiding marks
  // the WebContents HIDDEN, which throttles requestAnimationFrame, applies
  // background timer throttling, and stops compositing/painting -- so a
  // thumbnailed or covered browser stops competing with the foreground app for
  // the Krait cores. Showing restores VISIBLE.
  //
  // Only stamp the Active Frame cover when Navigator actually backgrounds us.
  // Updating the cover while fullscreen made Navigator report THUMBNAIL, which
  // hid the page and aborted googlevideo (YouTube "Playback failed").
  Shell* shell = nullptr;
  {
    base::AutoLock lock(g_chrome_lock);
    shell = g_active_shell;
  }
  if (!shell || !shell->web_contents())
    return;
  const GURL url = shell->web_contents()->GetVisibleURL();
  const std::string host = url.host();
  const bool keep_visible =
      host.find("youtube.com") != std::string::npos ||
      host.find("googlevideo.com") != std::string::npos;
  if (!visible)
    BerryUpdateWindowCover(url);
  if (!visible && keep_visible) {
    shell->web_contents()->UpdateWebContentsVisibility(Visibility::VISIBLE);
    return;
  }
  shell->web_contents()->UpdateWebContentsVisibility(
      visible ? Visibility::VISIBLE : Visibility::HIDDEN);
}

void QnxExitCallback() {
  // The Navigator (app swipe-up / close) asked us to exit. Graceful teardown
  // below can stall on a busy in-process renderer (single-process mode) or a
  // stuck present/RunUntilIdle loop, which is why content_shell sometimes
  // survived after the window closed. Arm an independent hard-exit watchdog so
  // the process is guaranteed to die shortly regardless of teardown progress.
  const char* pgo = getenv("BERRY_PGO_COLLECT");
  const bool pgo_collect = pgo && pgo[0] == '1';
  if (!pgo_collect)
    base::StartQnxExitWatchdog(3000);
  Shell::Shutdown();
  if (pgo_collect) {
    // Flush LLVM instrumentation profiles before exit (atexit is skipped by
    // _exit). Used during berry-pgo-collect.enable collection sessions.
    __llvm_profile_write_file();
    exit(0);
  }
  // If we got here cleanly the main loop quit closure already ran; make the
  // exit immediate rather than waiting on any remaining loop iterations.
  _exit(0);
}

}  // namespace

struct ShellPlatformDelegate::ShellData {
  gfx::NativeWindow window;
  std::unique_ptr<ShellPlatformDataAura> extra_aura;
  std::string group;
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
  ui::SetQnxScreenVisibilityCallback(QnxVisibilityCallback);
  ui::SetQnxScreenInvokeUriCallback(QnxInvokeUriCallback);
  ui::SetQnxScreenActiveGroupCallback(QnxActiveGroupCallback);
  navigator_set_close_prompt("Berry Browser", "Close Berry Browser?");
}

void ShellPlatformDelegate::CreatePlatformWindow(
    Shell* shell,
    const gfx::Size& initial_size) {
  DCHECK(!base::Contains(shell_data_map_, shell));
  ShellData& shell_data = shell_data_map_[shell];
  // One Screen application window (Navigator limit). Extra Shells are extra
  // WebContents in this same Aura host — swapped by BerryShowShell.
  platform_->aura->ResizeWindow(initial_size);
  shell_data.window = platform_->aura->host()->window();
  if (auto* wm = ui::GetQnxScreenWindowManager()) {
    if (auto* w = wm->GetLastAddedWindow())
      shell_data.group = w->group_name();
  }
  if (!shell_data.group.empty())
    g_group_to_shell[shell_data.group] = shell;

  {
    base::AutoLock lock(g_chrome_lock);
    g_active_shell = shell;
    int rw = initial_size.width() > 0 ? initial_size.width() : 720;
    int rh = initial_size.height() > 0 ? initial_size.height() : 720;
    ui::QnxScreenGetRenderSize(&rw, &rh);
    if (!BerryAppModeNoChrome() && !g_chrome)
      g_chrome = std::make_unique<BerryBrowserChrome>(rw, rh);
    g_url_bar_editing = false;
  }
}

gfx::NativeWindow ShellPlatformDelegate::GetNativeWindow(Shell* shell) {
  DCHECK(base::Contains(shell_data_map_, shell));
  ShellData& shell_data = shell_data_map_[shell];
  return shell_data.window;
}

void ShellPlatformDelegate::CleanUp(Shell* shell) {
  DCHECK(base::Contains(shell_data_map_, shell));
  auto it = shell_data_map_.find(shell);
  if (it != shell_data_map_.end() && !it->second.group.empty())
    g_group_to_shell.erase(it->second.group);
  if (g_active_shell == shell) {
    base::AutoLock lock(g_chrome_lock);
    g_active_shell = nullptr;
    g_url_bar_editing = false;
    for (Shell* s : Shell::windows()) {
      if (s != shell) {
        g_active_shell = s;
        break;
      }
    }
    if (!g_active_shell)
      g_chrome.reset();
  }
  shell_data_map_.erase(shell);
}

void ShellPlatformDelegate::SetContents(Shell* shell) {
  aura::Window* content = shell->web_contents()->GetNativeView();
  aura::Window* parent = platform_->aura->host()->window();
  if (!parent->Contains(content))
    parent->AddChild(content);

  {
    base::AutoLock lock(g_chrome_lock);
    auto* rwhv = shell->web_contents()->GetRenderWidgetHostView();
    if (rwhv) {
      if (g_chrome) {
        rwhv->SetSize(g_chrome->GetWebContentBounds().size());
      } else {
        int rw = 0, rh = 0;
        ui::QnxScreenGetRenderSize(&rw, &rh);
        if (rw > 0 && rh > 0)
          rwhv->SetSize(gfx::Size(rw, rh));
      }
    }
  }
  BerryShowShell(shell);
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
  if (!g_chrome || shell != g_active_shell)
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
  if (!g_chrome || g_url_bar_editing || shell != g_active_shell)
    return;
  const std::string spec = url.spec();
  if (spec == g_last_chrome_url)
    return;
  g_last_chrome_url = spec;
  g_chrome->SetUrl(spec);
  // While subresources load, DDG updates the visible URL frequently; repainting
  // the chrome overlay on every update contends with PresentCanvas at ~60fps.
  if (g_last_loading) {
    const base::TimeTicks now = base::TimeTicks::Now();
    if (!g_last_chrome_url_repaint.is_null() &&
        (now - g_last_chrome_url_repaint) < base::Milliseconds(250)) {
      return;
    }
    g_last_chrome_url_repaint = now;
  }
  RepaintChrome();
}

void ShellPlatformDelegate::SetIsLoading(Shell* shell, bool loading) {
  base::AutoLock lock(g_chrome_lock);
  if (!g_chrome || shell != g_active_shell)
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
