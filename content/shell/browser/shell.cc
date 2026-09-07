// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/shell/browser/shell.h"

#include <stddef.h>
#include <cstdio>

#include <map>
#include <memory>
#include <string>
#include <utility>

#if BUILDFLAG(IS_QNX)
#include <bps/bps.h>
#include <bps/navigator.h>
#include <bps/navigator_invoke.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include "base/qnx_pump_activity.h"
#endif
#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/values.h"
#include "base/location.h"
#include "base/no_destructor.h"
#include "base/run_loop.h"
#include "base/strings/escape.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "base/qnx_berry_daemon.h"
#include "base/qnx_hard_watchdog.h"
#include "base/qnx_trace.h"
#include "components/custom_handlers/protocol_handler.h"
#include "components/custom_handlers/protocol_handler_registry.h"
#include "components/custom_handlers/simple_protocol_handler_registry_factory.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/color_chooser.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/file_select_listener.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/picture_in_picture_window_controller.h"
#include "content/public/browser/presentation_receiver_flags.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/renderer_preferences_util.h"
#include "content/public/browser/visibility.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/content_switches.h"
#include "content/shell/app/resource.h"
#include "content/shell/browser/berry_geolocation_qnx.h"
#include "content/shell/browser/berry_maps_qnx.h"
#include "content/shell/browser/shell_content_browser_client.h"
#include "third_party/blink/public/common/user_agent/user_agent_metadata.h"
#include "content/shell/browser/shell_devtools_frontend.h"
#include "content/shell/browser/shell_javascript_dialog_manager.h"
#include "content/shell/common/shell_switches.h"
#include "media/media_buildflags.h"
#include "third_party/blink/public/common/peerconnection/webrtc_ip_handling_policy.h"
#include "third_party/blink/public/common/renderer_preferences/renderer_preferences.h"
#include "third_party/blink/public/mojom/choosers/file_chooser.mojom-forward.h"
#include "third_party/blink/public/mojom/window_features/window_features.mojom.h"
#include "url/url_constants.h"

namespace content {

#if BUILDFLAG(IS_QNX)
namespace {
}  // namespace
#endif

namespace {
// Null until/unless the default main message loop is running.
base::OnceClosure& GetMainMessageLoopQuitClosure() {
  static base::NoDestructor<base::OnceClosure> closure;
  return *closure;
}

#if BUILDFLAG(IS_QNX)
constexpr int kQnxWatchdogSlackMs = 15000;
#endif  // BUILDFLAG(IS_QNX)

constexpr int kDefaultTestWindowWidthDip = 800;
constexpr int kDefaultTestWindowHeightDip = 600;

// Owning pointer. We can not use unique_ptr as a global. That introduces a
// static constructor/destructor.
// Acquired in Shell::Init(), released in Shell::Shutdown().
ShellPlatformDelegate* g_platform;
}  // namespace

std::vector<Shell*> Shell::windows_;
base::OnceCallback<void(Shell*)> Shell::shell_created_callback_;

Shell::Shell(std::unique_ptr<WebContents> web_contents,
             bool should_set_delegate)
    : WebContentsObserver(web_contents.get()),
      web_contents_(std::move(web_contents)) {
  if (should_set_delegate)
    web_contents_->SetDelegate(this);

  if (!switches::IsRunWebTestsSwitchPresent()) {
    UpdateFontRendererPreferencesFromSystemSettings(
        web_contents_->GetMutableRendererPrefs());
  }

  windows_.push_back(this);

  if (shell_created_callback_)
    std::move(shell_created_callback_).Run(this);
}

Shell::~Shell() {
  g_platform->CleanUp(this);

  for (size_t i = 0; i < windows_.size(); ++i) {
    if (windows_[i] == this) {
      windows_.erase(windows_.begin() + i);
      break;
    }
  }

  web_contents_->SetDelegate(nullptr);
  web_contents_.reset();

  if (windows().empty())
    g_platform->DidCloseLastWindow();
}

Shell* Shell::CreateShell(std::unique_ptr<WebContents> web_contents,
                          const gfx::Size& initial_size,
                          bool should_set_delegate) {
  WebContents* raw_web_contents = web_contents.get();
  Shell* shell = new Shell(std::move(web_contents), should_set_delegate);
  g_platform->CreatePlatformWindow(shell, initial_size);

  // Note: Do not make RenderFrameHost or RenderViewHost specific state changes
  // here, because they will be forgotten after a cross-process navigation. Use
  // RenderFrameCreated or RenderViewCreated instead.
  if (switches::IsRunWebTestsSwitchPresent()) {
    raw_web_contents->GetMutableRendererPrefs()->use_custom_colors = false;
    raw_web_contents->SyncRendererPrefs();
  }

  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  if (command_line->HasSwitch(switches::kForceWebRtcIPHandlingPolicy)) {
    raw_web_contents->GetMutableRendererPrefs()->webrtc_ip_handling_policy =
        command_line->GetSwitchValueASCII(
            switches::kForceWebRtcIPHandlingPolicy);
  }

  g_platform->SetContents(shell);
  g_platform->DidCreateOrAttachWebContents(shell, raw_web_contents);
  // If the RenderFrame was created during WebContents construction (as happens
  // for windows opened from the renderer) then the Shell won't hear about the
  // main frame being created as a WebContentsObservers. This gives the delegate
  // a chance to act on the main frame accordingly.
  if (raw_web_contents->GetPrimaryMainFrame()->IsRenderFrameLive())
    g_platform->MainFrameCreated(shell);

#if BUILDFLAG(IS_QNX)
  // Seed GeolocationContext override before any page JS can bind geolocation.
  BerryPreseedGeolocationContext(raw_web_contents);
#endif

  return shell;
}

// static
void Shell::SetMainMessageLoopQuitClosure(base::OnceClosure quit_closure) {
  GetMainMessageLoopQuitClosure() = std::move(quit_closure);
}

// static
void Shell::QuitMainMessageLoopForTesting() {
  auto& quit_loop = GetMainMessageLoopQuitClosure();
  if (quit_loop)
    std::move(quit_loop).Run();
}

// static
void Shell::SetShellCreatedCallback(
    base::OnceCallback<void(Shell*)> shell_created_callback) {
  DCHECK(!shell_created_callback_);
  shell_created_callback_ = std::move(shell_created_callback);
}

// static
bool Shell::ShouldHideToolbar() {
  return base::CommandLine::ForCurrentProcess()->HasSwitch(
      switches::kContentShellHideToolbar);
}

// static
Shell* Shell::FromWebContents(WebContents* web_contents) {
  for (Shell* window : windows_) {
    if (window->web_contents() && window->web_contents() == web_contents) {
      return window;
    }
  }
  return nullptr;
}

// static
void Shell::Initialize(std::unique_ptr<ShellPlatformDelegate> platform) {
  DCHECK(!g_platform);
  g_platform = platform.release();
  g_platform->Initialize(GetShellDefaultSize());
}

// static
void Shell::Shutdown() {
  if (!g_platform)  // Shutdown has already been called.
    return;

  DevToolsAgentHost::DetachAllClients();

  while (!Shell::windows().empty())
    Shell::windows().back()->Close();

  delete g_platform;
  g_platform = nullptr;

  for (auto it = RenderProcessHost::AllHostsIterator(); !it.IsAtEnd();
       it.Advance()) {
    it.GetCurrentValue()->DisableRefCounts();
  }
  auto& quit_loop = GetMainMessageLoopQuitClosure();
  if (quit_loop)
    std::move(quit_loop).Run();

  // Pump the message loop to allow window teardown tasks to run.
  base::RunLoop().RunUntilIdle();
}

gfx::Size Shell::AdjustWindowSize(const gfx::Size& initial_size) {
  if (!initial_size.IsEmpty())
    return initial_size;
  return GetShellDefaultSize();
}

// static
Shell* Shell::CreateNewWindow(BrowserContext* browser_context,
                              const GURL& url,
                              const scoped_refptr<SiteInstance>& site_instance,
                              const gfx::Size& initial_size) {
  QNX_TRACE_MSG("QNX:Shell:1 CreateNewWindow\n");
  WebContents::CreateParams create_params(browser_context, site_instance);
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kForcePresentationReceiverForTesting)) {
    create_params.starting_sandbox_flags = kPresentationReceiverSandboxFlags;
  }
  std::unique_ptr<WebContents> web_contents =
      WebContents::Create(create_params);
  QNX_TRACE_MSG("QNX:Shell:2 WebContents\n");
  Shell* shell =
      CreateShell(std::move(web_contents), AdjustWindowSize(initial_size),
                  true /* should_set_delegate */);
  QNX_TRACE_MSG("QNX:Shell:3 CreateShell\n");
  /* Navigator may deliver NAVIGATOR_WINDOW_STATE after first paint; until then
   * WebContents defaults to hidden and YouTube/mobile players refuse to start
   * stream fetch (no get_watch / googlevideo). Assume visible at launch. */
  shell->web_contents()->UpdateWebContentsVisibility(Visibility::VISIBLE);

  if (!url.is_empty()) {
#if BUILDFLAG(IS_QNX)
    // Navigation must not start during PreMainMessageLoopRun: the IO thread
    // cannot service network-service Mojo until the UI message loop runs, which
    // otherwise queues the first URLLoader for ~20s after FactoryStart.
    Shell* shell_ptr = shell;
    GURL load_url = url;
    GetUIThreadTaskRunner({})->PostTask(
        FROM_HERE, base::BindOnce(
                       [](Shell* s, const GURL& u) {
                         QNX_NAV_LOG_FMT("BerryNav: DeferredLoadURL \"%s\"\n",
                                         u.spec().substr(0, 120).c_str());
                         s->LoadURL(u);
                       },
                       base::Unretained(shell_ptr), load_url));
#else
    shell->LoadURL(url);
#endif
  }
  QNX_TRACE_MSG("QNX:Shell:4 LoadURL done\n");
  return shell;
}

void Shell::RenderFrameCreated(RenderFrameHost* frame_host) {
  if (frame_host == web_contents_->GetPrimaryMainFrame())
    g_platform->MainFrameCreated(this);
}

void Shell::LoadURL(const GURL& url) {
  LoadURLForFrame(
      url, std::string(),
      ui::PageTransitionFromInt(ui::PAGE_TRANSITION_TYPED |
                                ui::PAGE_TRANSITION_FROM_ADDRESS_BAR));
}

void Shell::LoadURLForFrame(const GURL& url,
                            const std::string& frame_name,
                            ui::PageTransition transition_type) {
  QNX_TRACE_MSG("QNX:LoadURL:1 enter\n");
#if BUILDFLAG(IS_QNX)
  base::MarkQnxProcessPumpActive();
  std::string spec = url.spec();
  if (spec.size() > 120)
    spec = spec.substr(0, 120);
  QNX_NAV_LOG_FMT("BerryNav: LoadURL \"%s\"\n", spec.c_str());
#endif
  NavigationController::LoadURLParams params(url);
  params.frame_name = frame_name;
  params.transition_type = transition_type;
  QNX_TRACE_MSG("QNX:LoadURL:2 params\n");
  web_contents_->GetController().LoadURLWithParams(params);
  QNX_TRACE_MSG("QNX:LoadURL:3 done\n");
}

void Shell::LoadDataWithBaseURL(const GURL& url,
                                const std::string& data,
                                const GURL& base_url) {
  bool load_as_string = false;
  LoadDataWithBaseURLInternal(url, data, base_url, load_as_string);
}

#if BUILDFLAG(IS_ANDROID)
void Shell::LoadDataAsStringWithBaseURL(const GURL& url,
                                        const std::string& data,
                                        const GURL& base_url) {
  bool load_as_string = true;
  LoadDataWithBaseURLInternal(url, data, base_url, load_as_string);
}
#endif

void Shell::LoadDataWithBaseURLInternal(const GURL& url,
                                        const std::string& data,
                                        const GURL& base_url,
                                        bool load_as_string) {
#if !BUILDFLAG(IS_ANDROID)
  DCHECK(!load_as_string);  // Only supported on Android.
#endif

  NavigationController::LoadURLParams params(GURL::EmptyGURL());
  const std::string data_url_header = "data:text/html;charset=utf-8,";
  if (load_as_string) {
    params.url = GURL(data_url_header);
    std::string data_url_as_string = data_url_header + data;
#if BUILDFLAG(IS_ANDROID)
    params.data_url_as_string = base::MakeRefCounted<base::RefCountedString>(
        std::move(data_url_as_string));
#endif
  } else {
    params.url = GURL(data_url_header + data);
  }

  params.load_type = NavigationController::LOAD_TYPE_DATA;
  params.base_url_for_data_url = base_url;
  params.virtual_url_for_data_url = url;
  params.override_user_agent = NavigationController::UA_OVERRIDE_FALSE;
  web_contents_->GetController().LoadURLWithParams(params);
}

void Shell::AddNewContents(WebContents* source,
                           std::unique_ptr<WebContents> new_contents,
                           const GURL& target_url,
                           WindowOpenDisposition disposition,
                           const blink::mojom::WindowFeatures& window_features,
                           bool user_gesture,
                           bool* was_blocked) {
  CreateShell(
      std::move(new_contents), AdjustWindowSize(window_features.bounds.size()),
      !delay_popup_contents_delegate_for_testing_ /* should_set_delegate */);
}

void Shell::GoBackOrForward(int offset) {
  web_contents_->GetController().GoToOffset(offset);
}

void Shell::Reload() {
  web_contents_->GetController().Reload(ReloadType::NORMAL, false);
}

void Shell::ReloadBypassingCache() {
  web_contents_->GetController().Reload(ReloadType::BYPASSING_CACHE, false);
}

void Shell::Stop() {
  web_contents_->Stop();
}

void Shell::UpdateNavigationControls(bool should_show_loading_ui) {
  int current_index = web_contents_->GetController().GetCurrentEntryIndex();
  int max_index = web_contents_->GetController().GetEntryCount() - 1;

  g_platform->EnableUIControl(this, ShellPlatformDelegate::BACK_BUTTON,
                              current_index > 0);
  g_platform->EnableUIControl(this, ShellPlatformDelegate::FORWARD_BUTTON,
                              current_index < max_index);
  g_platform->EnableUIControl(
      this, ShellPlatformDelegate::STOP_BUTTON,
      should_show_loading_ui && web_contents_->IsLoading());
}

void Shell::ShowDevTools() {
  if (!devtools_frontend_) {
    auto* devtools_frontend = ShellDevToolsFrontend::Show(web_contents());
    devtools_frontend_ = devtools_frontend->GetWeakPtr();
  }

  devtools_frontend_->Activate();
}

void Shell::CloseDevTools() {
  if (!devtools_frontend_)
    return;
  devtools_frontend_->Close();
  devtools_frontend_ = nullptr;
}

void Shell::ResizeWebContentForTests(const gfx::Size& content_size) {
  g_platform->ResizeWebContent(this, content_size);
}

gfx::NativeView Shell::GetContentView() {
  if (!web_contents_)
    return gfx::NativeView();
  return web_contents_->GetNativeView();
}

#if !BUILDFLAG(IS_ANDROID)
gfx::NativeWindow Shell::window() {
  return g_platform->GetNativeWindow(this);
}
#endif

#if BUILDFLAG(IS_MAC)
void Shell::ActionPerformed(int control) {
  switch (control) {
    case IDC_NAV_BACK:
      GoBackOrForward(-1);
      break;
    case IDC_NAV_FORWARD:
      GoBackOrForward(1);
      break;
    case IDC_NAV_RELOAD:
      Reload();
      break;
    case IDC_NAV_STOP:
      Stop();
      break;
  }
}

void Shell::URLEntered(const std::string& url_string) {
  if (!url_string.empty()) {
    GURL url(url_string);
    if (!url.has_scheme())
      url = GURL("http://" + url_string);
    LoadURL(url);
  }
}
#endif

WebContents* Shell::OpenURLFromTab(WebContents* source,
                                   const OpenURLParams& params) {
  WebContents* target = nullptr;
  switch (params.disposition) {
    case WindowOpenDisposition::CURRENT_TAB:
      target = source;
      break;

    // Normally, the difference between NEW_POPUP and NEW_WINDOW is that a popup
    // should have no toolbar, no status bar, no menu bar, no scrollbars and be
    // not resizable.  For simplicity and to enable new testing scenarios in
    // content shell and web tests, popups don't get special treatment below
    // (i.e. they will have a toolbar and other things described here).
    case WindowOpenDisposition::NEW_POPUP:
    case WindowOpenDisposition::NEW_WINDOW:
    // content_shell doesn't really support tabs, but some web tests use
    // middle click (which translates into kNavigationPolicyNewBackgroundTab),
    // so we treat the cases below just like a NEW_WINDOW disposition.
    case WindowOpenDisposition::NEW_BACKGROUND_TAB:
    case WindowOpenDisposition::NEW_FOREGROUND_TAB: {
      Shell* new_window =
          Shell::CreateNewWindow(source->GetBrowserContext(),
                                 GURL(),  // Don't load anything just yet.
                                 params.source_site_instance,
                                 gfx::Size());  // Use default size.
      target = new_window->web_contents();
      break;
    }

    // No tabs in content_shell:
    case WindowOpenDisposition::SINGLETON_TAB:
    // No incognito mode in content_shell:
    case WindowOpenDisposition::OFF_THE_RECORD:
    // TODO(lukasza): Investigate if some web tests might need support for
    // SAVE_TO_DISK disposition.  This would probably require that
    // WebTestControlHost always sets up and cleans up a temporary directory
    // as the default downloads destinations for the duration of a test.
    case WindowOpenDisposition::SAVE_TO_DISK:
    // Ignoring requests with disposition == IGNORE_ACTION...
    case WindowOpenDisposition::IGNORE_ACTION:
    default:
      return nullptr;
  }

  target->GetController().LoadURLWithParams(
      NavigationController::LoadURLParams(params));
  return target;
}

void Shell::LoadingStateChanged(WebContents* source,
                                bool should_show_loading_ui) {
  UpdateNavigationControls(should_show_loading_ui);
  g_platform->SetIsLoading(this, source->IsLoading());
}

#if BUILDFLAG(IS_ANDROID)
void Shell::SetOverlayMode(bool use_overlay_mode) {
  g_platform->SetOverlayMode(this, use_overlay_mode);
}
#endif

void Shell::EnterFullscreenModeForTab(
    RenderFrameHost* requesting_frame,
    const blink::mojom::FullscreenOptions& options) {
  ToggleFullscreenModeForTab(WebContents::FromRenderFrameHost(requesting_frame),
                             true);
}

void Shell::ExitFullscreenModeForTab(WebContents* web_contents) {
  ToggleFullscreenModeForTab(web_contents, false);
}

void Shell::ToggleFullscreenModeForTab(WebContents* web_contents,
                                       bool enter_fullscreen) {
#if BUILDFLAG(IS_ANDROID) || BUILDFLAG(IS_IOS)
  g_platform->ToggleFullscreenModeForTab(this, web_contents, enter_fullscreen);
#endif
  if (is_fullscreen_ != enter_fullscreen) {
    is_fullscreen_ = enter_fullscreen;
    web_contents->GetPrimaryMainFrame()
        ->GetRenderViewHost()
        ->GetWidget()
        ->SynchronizeVisualProperties();
  }
}

bool Shell::IsFullscreenForTabOrPending(const WebContents* web_contents) {
#if BUILDFLAG(IS_ANDROID) || BUILDFLAG(IS_IOS)
  return g_platform->IsFullscreenForTabOrPending(this, web_contents);
#else
  return is_fullscreen_;
#endif
}

blink::mojom::DisplayMode Shell::GetDisplayMode(
    const WebContents* web_contents) {
  // TODO: should return blink::mojom::DisplayModeFullscreen wherever user puts
  // a browser window into fullscreen (not only in case of renderer-initiated
  // fullscreen mode): crbug.com/476874.
  return IsFullscreenForTabOrPending(web_contents)
             ? blink::mojom::DisplayMode::kFullscreen
             : blink::mojom::DisplayMode::kBrowser;
}

#if !BUILDFLAG(IS_ANDROID)
void Shell::RegisterProtocolHandler(RenderFrameHost* requesting_frame,
                                    const std::string& protocol,
                                    const GURL& url,
                                    bool user_gesture) {
  BrowserContext* context = requesting_frame->GetBrowserContext();
  if (context->IsOffTheRecord())
    return;

  custom_handlers::ProtocolHandler handler =
      custom_handlers::ProtocolHandler::CreateProtocolHandler(
          protocol, url, GetProtocolHandlerSecurityLevel(requesting_frame));

  // The parameters's normalization process defined in the spec has been already
  // applied in the WebContentImpl class, so at this point it shouldn't be
  // possible to create an invalid handler.
  // https://html.spec.whatwg.org/multipage/system-state.html#normalize-protocol-handler-parameters
  DCHECK(handler.IsValid());

  custom_handlers::ProtocolHandlerRegistry* registry = custom_handlers::
      SimpleProtocolHandlerRegistryFactory::GetForBrowserContext(context, true);
  DCHECK(registry);
  if (registry->SilentlyHandleRegisterHandlerRequest(handler))
    return;

  if (!user_gesture && !windows_.empty()) {
    // TODO(jfernandez): This is not strictly needed, but we need a way to
    // inform the observers in browser tests that the request has been
    // cancelled, to avoid timeouts. Chrome just holds the handler as pending in
    // the PageContentSettingsDelegate, but we don't have such thing in the
    // Content Shell.
    registry->OnDenyRegisterProtocolHandler(handler);
    return;
  }

  // FencedFrames can not register to handle any protocols.
  if (requesting_frame->IsNestedWithinFencedFrame()) {
    registry->OnIgnoreRegisterProtocolHandler(handler);
    return;
  }

  // TODO(jfernandez): Are we interested at all on using the
  // PermissionRequestManager in the ContentShell ?
  if (registry->registration_mode() ==
      custom_handlers::RphRegistrationMode::kAutoAccept) {
    registry->OnAcceptRegisterProtocolHandler(handler);
  }
}
#endif

void Shell::RequestToLockMouse(WebContents* web_contents,
                               bool user_gesture,
                               bool last_unlocked_by_target) {
  // Give the platform a chance to handle the lock request, if it doesn't
  // indicate it handled it, allow the request.
  if (!g_platform->HandleRequestToLockMouse(this, web_contents, user_gesture,
                                            last_unlocked_by_target)) {
    web_contents->GotResponseToLockMouseRequest(
        blink::mojom::PointerLockResult::kSuccess);
  }
}

void Shell::Close() {
  // Shell is "self-owned" and destroys itself. The ShellPlatformDelegate
  // has the chance to co-opt this and do its own destruction.
  if (!g_platform->DestroyShell(this))
    delete this;
}

void Shell::CloseContents(WebContents* source) {
  Close();
}

bool Shell::CanOverscrollContent() {
#if defined(USE_AURA)
  return true;
#else
  return false;
#endif
}

void Shell::NavigationStateChanged(WebContents* source,
                                   InvalidateTypes changed_flags) {
  if (changed_flags & INVALIDATE_TYPE_URL)
    g_platform->SetAddressBarURL(this, source->GetVisibleURL());
}

void Shell::ResetForBerryDaemonLoad() {
  dom_already_dumped_ = false;
  timeout_armed_ = false;
  dump_timer_.Stop();
}

void Shell::DumpDomAndExit(RenderFrameHost* rfh) {
  if (dom_already_dumped_)
    return;
  dom_already_dumped_ = true;
  dump_timer_.Stop();
  QNX_TRACE_MSG("QNX:Shell:DumpDomAndExit\n");

  const std::u16string script = u"document.documentElement.outerHTML";
  rfh->ExecuteJavaScriptForTests(
      script,
      base::BindOnce(
          [](base::Value value) {
            QNX_TRACE_MSG("QNX:Shell:DumpDom callback\n");
            std::string html;
            if (value.is_string())
              html = value.GetString();
            if (base::QnxBerryDaemonEnabled()) {
              base::QnxBerryDaemonEmitHtml(
                  base::span<const char>(html.data(), html.size()));
              return;
            }
            base::CancelQnxHardWatchdog();
            write(1, html.c_str(), html.size());
            write(1, "\n", 1);
#if BUILDFLAG(IS_QNX)
            _exit(0);
#else
            printf("%s\n", html.c_str());
            fflush(stdout);
#endif
          }));
}

void Shell::MaybeArmDumpTimeout(NavigationHandle* navigation_handle) {
  if (!navigation_handle->IsInPrimaryMainFrame())
    return;
  if (!navigation_handle->HasCommitted())
    return;

  auto* cmd = base::CommandLine::ForCurrentProcess();
  if (!cmd->HasSwitch(switches::kDumpDom))
    return;

  const GURL& nav_url = navigation_handle->GetURL();
  if (nav_url.IsAboutBlank() || nav_url.spec() == "about:blank")
    return;

  // A real page committed: the boot-phase init-deadlock watchdog is no longer
  // needed; the post-commit --timeout watchdog (if armed) covers the rest.
  base::CancelQnxBootWatchdog();

  if (timeout_armed_)
    return;

  std::string timeout_str = cmd->GetSwitchValueASCII(switches::kTimeout);
  int timeout_ms = 0;
  if (!timeout_str.empty() && base::StringToInt(timeout_str, &timeout_ms) &&
      timeout_ms > 0) {
    timeout_armed_ = true;
    QNX_TRACE_FMT("QNX:Shell:StartTimer %dms (post-commit)\n", timeout_ms);
    dump_timer_.Start(FROM_HERE, base::Milliseconds(timeout_ms),
                      base::BindOnce(&Shell::OnTimeout,
                                     base::Unretained(this)));
#if BUILDFLAG(IS_QNX)
    if (!base::QnxBerryDaemonEnabled())
      base::StartQnxHardWatchdog(timeout_ms + kQnxWatchdogSlackMs);
#endif
  }
}

void Shell::OnTimeout() {
  QNX_TRACE_MSG("QNX:Shell:OnTimeout fired\n");
  if (dom_already_dumped_)
    return;
#if BUILDFLAG(IS_QNX)
  // Parse-time dump in frame_loader.cc handles headless output on QNX. The JS
  // round-trip via DumpDomAndExit hangs on heavy pages (google.com) while the
  // network/parser may still be in flight. HardWatchdog (with extended slack)
  // ensures we eventually exit if parse never completes.
  if (!base::QnxBerryDaemonEnabled())
    return;
#endif
  auto* rfh = web_contents_->GetPrimaryMainFrame();
  if (rfh)
    DumpDomAndExit(rfh);
}

#if BUILDFLAG(IS_QNX)
namespace {

GURL BerryIntentUrlToHttps(const GURL& url) {
  if (url.scheme() != "intent")
    return GURL();
  const std::string& spec = url.spec();
  constexpr char kPrefix[] = "intent://";
  if (!base::StartsWith(spec, kPrefix))
    return GURL();
  const size_t start = sizeof(kPrefix) - 1;
  const size_t end = spec.find("#Intent");
  const size_t len =
      (end == std::string::npos ? spec.size() : end) - start;
  if (len == 0)
    return GURL();
  return GURL("https://" + spec.substr(start, len));
}

// Hosts that refuse a mobile UA and must be served the desktop UA. WhatsApp Web
// and Messenger Web are the canonical cases: on mobile they show "use on your
// computer" instead of the chat UI.
bool BerryHostPrefersDesktopUA(const GURL& url) {
  const std::string host = url.host();
  if (host == "whatsapp.com" ||
      base::EndsWith(host, ".whatsapp.com",
                     base::CompareCase::INSENSITIVE_ASCII))
    return true;
  if (host == "messenger.com" ||
      base::EndsWith(host, ".messenger.com",
                     base::CompareCase::INSENSITIVE_ASCII))
    return true;
  // Messenger login/oauth/checkpoint flows redirect through www.facebook.com
  // (not m.facebook.com — that tile keeps the lightweight mobile site). Use
  // desktop UA for ALL www.facebook.com paths so verification ("finish signing
  // in on Facebook") works; path-gating left mobile UA and surfaced a bogus
  // "incorrect password" on messenger.com instead of the real checkpoint UI.
  if (host == "facebook.com" || host == "www.facebook.com")
    return true;
  // YouTube: desktop UA by default on www.youtube.com — progressive HTTPS
  // videoplayback (fmt=18) without SABR/PO tokens. Mobile/m.youtube uses SABR
  // which content_shell cannot decode. Opt into mobile with berry-youtube-mobile.enable.
  if (access("/accounts/1000/shared/misc/berry-youtube-mobile.enable",
             F_OK) == 0)
    return false;
  if (host == "youtube.com" || host == "www.youtube.com" ||
      host == "m.youtube.com" ||
      base::EndsWith(host, ".googlevideo.com",
                     base::CompareCase::INSENSITIVE_ASCII))
    return true;
  // Legacy opt-in for desktop watch experiments.
  if (access("/accounts/1000/shared/misc/berry-youtube-desktop.enable",
             F_OK) == 0) {
    if (host == "youtube.com" || host == "www.youtube.com" ||
        base::EndsWith(host, ".googlevideo.com",
                       base::CompareCase::INSENSITIVE_ASCII))
      return true;
  }
  // Google Maps on google.com/maps: desktop tactile fetches maps/vt vector tiles.
  // Build 56 mobile UA served Maps Lite (map_raster) but zero khms/vt tile
  // requests on QNX. Embedded Maps on third-party hosts stay mobile UA.
  if (access("/accounts/1000/shared/misc/berry-maps-mobile.enable", F_OK) == 0)
    return false;
  if (host == "maps.google.com")
    return true;
  if ((host == "google.com" || host == "www.google.com") &&
      base::StartsWith(url.path(), "/maps", base::CompareCase::SENSITIVE))
    return true;
  return false;
}

// In-app "Restart browser". Gracefully tears down, then re-execs the launcher in
// the SAME process (pid preserved, so the QNX/Navigator window-group model is
// identical to a normal launch). The launcher re-reads every marker, so settings
// that only apply at startup -- notably a new resolution (berry-x-*) -- take
// effect on restart. Mirrors QnxExitCallback but relaunches instead of exiting.
// Posted off the navigation observer callback so teardown doesn't run while a
// navigation is in flight. The host below must match home.html's restart link.
#if BUILDFLAG(IS_QNX)
extern "C" int __llvm_profile_write_file(void);

bool BerryPgoCollectEnabled() {
  const char* e = getenv("BERRY_PGO_COLLECT");
  return e && e[0] == '1';
}

void BerryWritePgoProfileIfCollecting() {
  if (BerryPgoCollectEnabled())
    __llvm_profile_write_file();
}
#endif

void BerryRestartRelaunch() {
#if BUILDFLAG(IS_QNX)
  if (!BerryPgoCollectEnabled())
    base::StartQnxExitWatchdog(3000);
#else
  base::StartQnxExitWatchdog(3000);
#endif
  Shell::Shutdown();
#if BUILDFLAG(IS_QNX)
  BerryWritePgoProfileIfCollecting();
#endif
  char exe[2048];
  exe[0] = '\0';
  int fd = open("/proc/self/exefile", O_RDONLY);
  if (fd >= 0) {
    ssize_t r = read(fd, exe, sizeof(exe) - 1);
    close(fd);
    if (r > 0) {
      exe[r] = '\0';
      while (r > 0 && (exe[r - 1] == '\n' || exe[r - 1] == '\r' ||
                       exe[r - 1] == ' ' || exe[r - 1] == '\0'))
        exe[--r] = '\0';
    }
  }
  if (exe[0]) {
    char* slash = strrchr(exe, '/');
    if (slash) {
      *slash = '\0';
      char launcher[2100];
      snprintf(launcher, sizeof(launcher), "%s/launcher", exe);
      char* args[] = {launcher, nullptr};
      execv(launcher, args);
    }
  }
  // Relaunch failed: just exit (Navigator closes the torn-down app).
  _exit(42);
}

// ---------------------------------------------------------------------------
// In-app Settings, rendered by the engine.
//
// Web pages (home.html) run sandboxed and cannot write the marker files in
// shared/misc that drive the launcher. content_shell CAN (it already writes its
// log there), so the Settings UI is generated and served by the engine:
//   - navigate to https://berry.settings/  -> render the live Settings page
//   - tap a toggle -> https://berry.set/?k=KEY&v=VAL -> engine writes the marker
//     and re-renders, so the page always reflects the real on-disk state.
// Most settings are startup flags, so the page tells the user to tap Restart to
// apply. The page is written to a file and loaded (avoids data: URL escaping).
// ---------------------------------------------------------------------------
const char kBerryMiscDir[] = "/accounts/1000/shared/misc/";

bool BerryHasMarker(const char* name) {
  std::string p = std::string(kBerryMiscDir) + name;
  return access(p.c_str(), F_OK) == 0;
}

// Matches launcher defaults + marker precedence (see apply_x_load_profile).
std::string BerryCurrentResKey() {
  if (BerryHasMarker("berry-x-420.enable"))
    return "420";
  if (BerryHasMarker("berry-x-540.enable"))
    return "540";
  if (BerryHasMarker("berry-x-720.enable"))
    return "720";
  if (BerryHasMarker("berry-x-1440.enable"))
    return "1440";
  return "540";
}

std::string BerryCurrentFpsKey() {
  if (BerryHasMarker("berry-x-perf.enable"))
    return "perf";
  if (BerryHasMarker("berry-x-lite.enable"))
    return "lite";
  if (BerryHasMarker("berry-x-fullfps.enable"))
    return "60";
  if (BerryHasMarker("berry-x-slow12.enable"))
    return "12";
  if (BerryHasMarker("berry-x-slow10.enable"))
    return "10";
  if (BerryHasMarker("berry-x-slow15.enable"))
    return "15";
  if (BerryHasMarker("berry-x-slow45.enable"))
    return "45";
  return "45";
}

bool BerryGpuEnabledByDefault() {
  return !BerryHasMarker("berry-gpu.disable");
}

bool BerryLowendEnabledByDefault() {
  return !BerryHasMarker("berry-lowend.disable");
}

bool BerryBlockTelemetryEnabled() {
  return !BerryHasMarker("berry-block.disable");
}

bool BerryPrivacyCutsEnabled() {
  return !BerryHasMarker("berry-privacy.enable");
}

bool BerryQuicEnabled() {
  // OPT-IN since build 68: QUIC proof verification fails on QNX
  // (CERTIFICATE_VERIFY_FAILED spam; CBC/googlevideo media timeouts), so
  // HTTP/3 is off unless explicitly enabled for debugging.
  return BerryHasMarker("berry-quic.enable");
}

bool BerryWasmLiftoffEnabled() {
  return BerryHasMarker("berry-wasm-liftoff.enable");
}

bool BerryStallPeakLogEnabled() {
  return !BerryHasMarker("berry-stall-log.disable");
}

void BerrySetMarker(const char* name, bool on) {
  std::string p = std::string(kBerryMiscDir) + name;
  if (on) {
    int fd = open(p.c_str(), O_WRONLY | O_CREAT, 0644);
    if (fd >= 0)
      close(fd);
  } else {
    unlink(p.c_str());
  }
}

std::string BerryReadTextMarker(const char* name) {
  std::string p = std::string(kBerryMiscDir) + name;
  std::string out;
  FILE* f = fopen(p.c_str(), "r");
  if (f) {
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
      out.append(buf, n);
    fclose(f);
  }
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r' ||
                          out.back() == ' ' || out.back() == '\t'))
    out.pop_back();
  return out;
}

std::string BerryCurrentDeviceKey() {
  std::string d = BerryReadTextMarker("berry-device");
  if (d.empty())
    return "auto";
  return d;
}

const char* BerryDeviceLabel(const std::string& id) {
  if (id == "auto")
    return "Auto";
  if (id == "classic" || id == "q20")
    return "Classic";
  if (id == "q10")
    return "Q10";
  if (id == "q5")
    return "Q5";
  if (id == "z10")
    return "Z10";
  if (id == "z30")
    return "Z30";
  if (id == "z3")
    return "Z3";
  if (id == "leap")
    return "Leap";
  return "Passport";
}

const char* BerryDevicePanelHint(const std::string& id) {
  if (id == "auto")
    return "panel detected at launch";
  if (id == "classic" || id == "q20" || id == "q10" || id == "q5")
    return "720\u00b2 panel";
  if (id == "z10")
    return "768\u00d71280";
  if (id == "z30" || id == "z3" || id == "leap")
    return "720\u00d71280";
  return "1440\u00b2 panel";
}

void BerrySetTextMarker(const char* name, const std::string& val) {
  std::string p = std::string(kBerryMiscDir) + name;
  if (val.empty()) {
    unlink(p.c_str());
    return;
  }
  FILE* f = fopen(p.c_str(), "w");
  if (f) {
    fwrite(val.data(), 1, val.size(), f);
    fclose(f);
  }
}

std::string BerryHtmlEscape(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      case '\'': o += "&#39;"; break;
      default: o += c;
    }
  }
  return o;
}

std::string BerryBuildSettingsHtml() {
  auto toggle = [](const char* label, const char* sub, const char* key,
                   bool on) -> std::string {
    std::string s = "<div class='row'><div class='lbl'><b>";
    s += label;
    s += "</b><i>";
    s += sub;
    s += "</i></div><a class='sw ";
    s += on ? "on" : "off";
    s += "' href='https://berry.set/?k=";
    s += key;
    s += "&v=";
    s += on ? "0" : "1";
    s += "'><span></span></a></div>";
    return s;
  };

  std::string res = BerryCurrentResKey();
  std::string fps = BerryCurrentFpsKey();
  std::string device = BerryCurrentDeviceKey();

  auto devbtn = [&](const char* val, const char* label,
                    const char* sub) -> std::string {
    std::string s = "<a class='res";
    if (device == val)
      s += " cur";
    s += "' href='https://berry.set/?k=device&v=";
    s += val;
    s += "'>";
    s += label;
    s += "<br><small>";
    s += sub;
    s += "</small></a>";
    return s;
  };

  auto resbtn = [&](const char* val, const char* label) -> std::string {
    std::string s = "<a class='res";
    if (res == val)
      s += " cur";
    s += "' href='https://berry.set/?k=res&v=";
    s += val;
    s += "'>";
    s += label;
    s += "</a>";
    return s;
  };

  auto fpsbtn = [&](const char* val, const char* label) -> std::string {
    std::string s = "<a class='res";
    if (fps == val)
      s += " cur";
    s += "' href='https://berry.set/?k=fps&v=";
    s += val;
    s += "'>";
    s += label;
    s += "</a>";
    return s;
  };

  std::string ua = BerryReadTextMarker("berry-ua");
  std::string home = BerryReadTextMarker("berry-home-url");

  std::string h;
  h += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  h += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  h += "<title>Berry Browser Settings</title><style>";
  h += "*{box-sizing:border-box;-webkit-tap-highlight-color:transparent;}";
  h += "html,body{margin:0;padding:0;background:#1a0012;color:#f3e6ef;"
       "font-family:-apple-system,'Slate Pro',Arial,sans-serif;}";
  h += ".wrap{max-width:720px;margin:0 auto;padding:20px 18px 40px;}";
  h += ".top{display:flex;align-items:center;gap:12px;margin:6px 0 18px;}";
  h += ".top h1{font-size:30px;margin:0;font-weight:700;}";
  h += ".top a.home{margin-left:auto;font-size:18px;color:#f3e6ef;"
       "text-decoration:none;border:1px solid #5e2750;padding:8px 14px;"
       "border-radius:10px;}";
  h += ".sec{font-size:14px;letter-spacing:.08em;text-transform:uppercase;"
       "color:#a08098;margin:22px 4px 8px;}";
  h += ".card{background:#2c001e;border:1px solid #5e2750;border-radius:14px;"
       "overflow:hidden;}";
  h += ".row{display:flex;align-items:center;padding:14px 16px;"
       "border-top:1px solid #5e2750;}";
  h += ".card .row:first-child{border-top:none;}";
  h += ".lbl{display:flex;flex-direction:column;gap:2px;}";
  h += ".lbl b{font-size:19px;font-weight:600;}";
  h += ".lbl i{font-size:13px;color:#c4a0b8;font-style:normal;}";
  h += ".sw{margin-left:auto;width:58px;height:32px;border-radius:18px;"
       "position:relative;flex:none;background:#3c0836;transition:.15s;}";
  h += ".sw span{position:absolute;top:3px;left:3px;width:26px;height:26px;"
       "border-radius:50%;background:#e8d0e0;transition:.15s;}";
  h += ".sw.on{background:#77216f;}.sw.on span{left:29px;background:#fff;}";
  h += ".resrow{display:flex;gap:10px;padding:14px 16px;flex-wrap:wrap;}";
  h += ".res{flex:1;text-align:center;padding:14px 0;border-radius:10px;"
       "background:#3c0836;color:#e8d0e0;text-decoration:none;font-size:18px;"
       "border:1px solid #5e2750;min-width:140px;}";
  h += ".res.cur{background:#77216f;color:#fff;border-color:#e95420;}";
  h += "form.tx{display:flex;gap:8px;padding:12px 16px;}";
  h += "form.tx input{flex:1;font-size:17px;padding:12px;border-radius:10px;"
       "border:1px solid #5e2750;background:#1a0012;color:#fff;outline:none;}";
  h += "form.tx button{font-size:17px;padding:0 18px;border:none;"
       "border-radius:10px;background:#77216f;color:#fff;font-weight:600;}";
  h += ".apply{display:block;margin:26px 0 0;text-align:center;font-size:22px;"
       "font-weight:700;padding:18px;border-radius:14px;background:#e95420;"
       "color:#fff;text-decoration:none;}";
  h += ".note{text-align:center;color:#c4a0b8;font-size:14px;margin:14px 4px 0;}";
  h += "</style></head><body><div class='wrap'>";
  h += "<div class='top'><h1>Settings</h1>"
       "<a class='home' href='https://berry.settings/'>\u21bb</a>"
       "<a class='home' href='https://berry.home/'>Home</a></div>";

  h += "<div class='sec'>Device</div><div class='card'>";
  h += "<div class='row'><div class='lbl'><b>";
  h += BerryDeviceLabel(device);
  h += "</b><i>Touch mapping + panel size (";
  h += BerryDevicePanelHint(device);
  h += "). Pick your phone model.</i></div></div>";
  h += "<div class='resrow'>";
  h += devbtn("auto", "Auto", "detect");
  h += devbtn("passport", "Passport", "1440 sq");
  h += devbtn("classic", "Classic", "720 sq");
  h += devbtn("q10", "Q10", "720 sq");
  h += "</div><div class='resrow'>";
  h += devbtn("q5", "Q5", "720 sq");
  h += devbtn("z10", "Z10", "768 wide");
  h += devbtn("z30", "Z30", "720 wide");
  h += devbtn("z3", "Z3", "720 wide");
  h += "</div><div class='resrow'>";
  h += devbtn("leap", "Leap", "720 wide");
  h += "</div></div>";

  h += "<div class='sec'>Display</div><div class='card'>";
  h += "<div class='resrow'>";
  h += resbtn("420", "420\u00b2<br><small>fast</small>");
  h += resbtn("540", "540\u00b2<br><small>balanced</small>");
  h += resbtn("720", "720\u00b2<br><small>sharp</small>");
  h += resbtn("1440", "1440<br><small>native</small>");
  h += "</div>";
  h += "<div class='row'><div class='lbl'><b>Resolution tier</b><i>Default "
       "540\u00b2 on Passport; scales render buffer for your device.</i></div>"
       "</div>";
  h += toggle("Dark mode", "Force dark rendering on all sites", "dark",
              BerryHasMarker("berry-dark.enable"));
  h += "</div>";

  h += "<div class='sec'>Frame rate</div><div class='card'>";
  h += "<div class='resrow'>";
  h += fpsbtn("60", "60<br><small>smooth</small>");
  h += fpsbtn("45", "45<br><small>balanced</small>");
  h += fpsbtn("perf", "Perf<br><small>540+12</small>");
  h += fpsbtn("15", "15<br><small>cool</small>");
  h += fpsbtn("lite", "Lite<br><small>420+12</small>");
  h += "</div></div>";

  h += "<div class='sec'>Content &amp; privacy</div><div class='card'>";
  h += toggle("Block images", "Don't load images (faster, less data)",
              "noimages", BerryHasMarker("berry-noimages.enable"));
  h += toggle("Disable JavaScript", "Static pages only", "nojs",
              BerryHasMarker("berry-nojs.enable"));
  h += toggle("Block ads &amp; trackers", "Built-in network blocklist",
              "adblock", !BerryHasMarker("berry-adblock.disable"));
  h += "</div>";

  h += "<div class='sec'>Network</div><div class='card'>";
  h += toggle("HTTP/3 (QUIC)", "Breaks video CDNs on QNX (default off)",
              "quic", BerryQuicEnabled());
  h += toggle("HTTP/1.1 only", "Fallback when HTTP/2 misbehaves", "http1",
              BerryHasMarker("berry-http1.enable"));
  h += toggle("Disable Alt-Svc", "Skip DNS HTTPS/SVCB upgrade hints", "altsvc",
              BerryHasMarker("berry-alt-svc.disable"));
  h += toggle("Block telemetry", "Block Meta analytics pixels (graph API allowed)",
              "block", BerryBlockTelemetryEnabled());
  h += "</div>";

  h += "<div class='sec'>YouTube</div><div class='card'>";
  h += toggle("Mobile YouTube", "Use mobile UI (default is desktop)", "ytmobile",
              BerryHasMarker("berry-youtube-mobile.enable"));
  h += "</div>";

  h += "<div class='sec'>Identity</div><div class='card'>";
  h += toggle("Desktop site", "Request desktop layout/UA", "desktop",
              BerryHasMarker("berry-desktop.enable"));
  h += "<form class='tx' action='https://berry.set/' method='get'>"
       "<input type='hidden' name='k' value='ua'>"
       "<input name='v' placeholder='Custom user-agent (blank = default)' "
       "value='";
  h += BerryHtmlEscape(ua);
  h += "'><button type='submit'>Set</button></form>";
  h += "</div>";

  h += "<div class='sec'>Performance</div><div class='card'>";
  h += toggle("GPU rendering", "EGL compositing (default on)", "gpu",
              BerryGpuEnabledByDefault());
  h += toggle("Low-end mode", "Smaller heaps/caches (default on)", "lowend",
              BerryLowendEnabledByDefault());
  h += toggle("Service Workers", "PWA app-shell caching (default on)", "sw",
              !BerryHasMarker("berry-sw.disable"));
  h += toggle("Disk &amp; media cache", "256MB HTTP/media cache (default on)",
              "diskcache", !BerryHasMarker("berry-disk-cache.disable"));
  h += toggle("Privacy sandbox cuts", "Disable FedCM/ads APIs (default on)",
              "privacy", BerryPrivacyCutsEnabled());
  h += toggle("WASM liftoff-only", "Faster WhatsApp load; off for Messenger login",
              "wasmliftoff", BerryWasmLiftoffEnabled());
  h += toggle("Stall peak log", "Log worst main-thread gap / 60s (default on)",
              "stalllog", BerryStallPeakLogEnabled());
  h += "</div>";

  h += "<div class='sec'>Developer</div><div class='card'>";
  h += toggle("Real microphone", "QSA mic capture (needs record_audio)", "mic",
              BerryHasMarker("berry-mic.enable"));
  h += toggle("Video debug log", "Verbose media logging to berry-kbd.log",
              "videodebug", BerryHasMarker("berry-video.debug"));
  h += toggle("Input debug log", "Touch/keyboard tracing (slow)", "kbddebug",
              BerryHasMarker("berry-kbd.debug"));
  h += toggle("Ship log on boot",
              "POST previous session's log to the endpoint below", "logship",
              !BerryHasMarker("berry-logship.disable"));
  h += "<form class='tx' action='https://berry.set/' method='get'>"
       "<input type='hidden' name='k' value='logshipurl'>"
       "<input name='v' placeholder='Log endpoint host[:port][/path] "
       "(blank = off)' value='";
  h += BerryHtmlEscape(BerryReadTextMarker("berry-logship-url"));
  h += "'><button type='submit'>Set</button></form>";
  h += "</div>";

  h += "<div class='sec'>Home screen shortcuts</div><div class='card'>";
  h += "<div class='row'><div class='lbl'><b>Add to Home Screen</b>"
       "<i>Puts a web link on the BB10 desktop. Tapping it opens that site "
       "in Berry Browser. Other apps can also Open http/https links here.</i>"
       "</div></div>";
  h += "<form class='tx' action='https://berry.pin/' method='get'>"
       "<input name='title' placeholder='Shortcut name'>"
       "<input name='url' placeholder='https://...'>"
       "<button type='submit'>Pin</button></form>";
  h += "</div>";

  h += "<div class='sec'>Share &amp; system</div><div class='card'>";
  h += "<div class='row'><div class='lbl'><b>Share last page</b>"
       "<i>Opens the BB10 Share card with the last website you visited "
       "(Messages, Remember, email, and other share targets).</i></div></div>";
  h += "<div class='resrow'><a class='res' href='https://berry.share/'>"
       "Share last page</a></div>";
  h += "<div class='row'><div class='lbl'><b>Active Frame</b>"
       "<i>Minimized card shows the Berry icon plus the current site name. "
       "mailto, tel, and sms links open the matching BB10 app.</i></div></div>";
  h += "</div>";

  h += "<div class='sec'>Start page</div><div class='card'>";
  h += "<form class='tx' action='https://berry.set/' method='get'>"
       "<input type='hidden' name='k' value='home'>"
       "<input name='v' placeholder='Start URL (blank = built-in home)' "
       "value='";
  h += BerryHtmlEscape(home);
  h += "'><button type='submit'>Set</button></form>";
  h += "</div>";

  h += "<a class='apply' href='https://berry.restart/'>Apply &amp; Restart</a>";
  h += "<div class='note'>Changes are saved instantly but take effect after a "
       "restart.</div>";
  h += "</div></body></html>";
  return h;
}

std::string BerryNativeDir() {
  char exe[2048];
  exe[0] = '\0';
  int fd = open("/proc/self/exefile", O_RDONLY);
  if (fd >= 0) {
    ssize_t r = read(fd, exe, sizeof(exe) - 1);
    close(fd);
    if (r > 0) {
      exe[r] = '\0';
      while (r > 0 && (exe[r - 1] == '\n' || exe[r - 1] == '\r' ||
                       exe[r - 1] == ' ' || exe[r - 1] == '\0'))
        exe[--r] = '\0';
    }
  }
  if (exe[0]) {
    char* slash = strrchr(exe, '/');
    if (slash)
      *slash = '\0';
  }
  return std::string(exe);
}

bool BerryCopyFile(const char* src, const char* dst) {
  FILE* in = fopen(src, "rb");
  if (!in)
    return false;
  FILE* out = fopen(dst, "wb");
  if (!out) {
    fclose(in);
    return false;
  }
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
    fwrite(buf, 1, n, out);
  fclose(in);
  fclose(out);
  return true;
}

std::string BerrySanitizeShortcutTitle(std::string t) {
  for (char& c : t) {
    if (c == '\n' || c == '\r' || c == '\t')
      c = ' ';
  }
  while (!t.empty() && t.front() == ' ')
    t.erase(t.begin());
  while (!t.empty() && t.back() == ' ')
    t.pop_back();
  if (t.size() > 32)
    t.resize(32);
  return t;
}

bool BerryAddHomeScreenShortcut(const std::string& title,
                                const std::string& url) {
  const std::string label = BerrySanitizeShortcutTitle(title);
  GURL g(url);
  if (label.empty() || !g.is_valid() ||
      !(g.SchemeIsHTTPOrHTTPS() || g.SchemeIsFile())) {
    QNX_NAV_LOG_FMT("BerryNav: pin reject title=\"%s\" url=\"%s\"\n",
                    label.c_str(), url.c_str());
    return false;
  }
  const std::string dir = BerryNativeDir();
  std::string src = dir + "/pin-icon.png";
  if (access(src.c_str(), R_OK) != 0)
    src = dir + "/icon.png";
  const char* shared = "/accounts/1000/shared/misc/berry-pin-icon.png";
  if (access(shared, R_OK) != 0)
    BerryCopyFile(src.c_str(), shared);
  const char* icon =
      (access(shared, R_OK) == 0) ? shared : src.c_str();
  const std::string invoke =
      "berrybrowser://open?u=" +
      base::EscapeQueryParamValue(g.spec(), /*use_plus=*/false) + "&win=1";

  char* err = nullptr;
  int rc = navigator_add_uri(icon, label.c_str(), "", invoke.c_str(), &err);
  if (rc != BPS_SUCCESS) {
    if (err) {
      bps_free(err);
      err = nullptr;
    }
    rc = navigator_add_uri(icon, label.c_str(), "media", invoke.c_str(), &err);
  }
  QNX_NAV_LOG_FMT("BerryNav: pin rc=%d title=\"%s\" invoke=\"%s\" err=%s\n", rc,
                  label.c_str(), invoke.c_str(), err ? err : "");
  if (err)
    bps_free(err);
  return rc == BPS_SUCCESS;
}

void BerryShowPinResult(Shell* shell,
                        bool ok,
                        const std::string& title,
                        const std::string& url) {
  std::string h =
      "<!doctype html><html><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>Home screen</title><style>"
      "body{margin:0;font-family:sans-serif;background:#1a0012;color:#f3e6ef;"
      "padding:28px 20px;}"
      "h1{font-size:26px;margin:0 0 10px;}"
      "p{color:#c4a0b8;font-size:16px;line-height:1.45;}"
      "a{display:block;margin-top:18px;text-align:center;padding:16px;"
      "border-radius:12px;text-decoration:none;font-weight:700;}"
      ".ok{background:#77216f;color:#fff;}"
      ".home{background:#3c0836;color:#e8d0e0;}"
      "</style></head><body>";
  if (ok) {
    h += "<h1>Pinned</h1><p><b>";
    h += BerryHtmlEscape(title);
    h += "</b> is on the BB10 home screen. Swipe to an empty tile to find it. "
         "Tap the icon to open ";
    h += BerryHtmlEscape(url);
    h += " in Berry Browser.</p>";
  } else {
    h += "<h1>Could not pin</h1><p>The shortcut was not created. Check the "
         "name and URL, then try Settings &rarr; Home screen shortcuts "
         "again.</p>";
  }
  h += "<a class='ok' href='https://berry.home/'>Back to Home</a>";
  h += "<a class='home' href='https://berry.settings/'>Settings</a>";
  h += "</body></html>";
  const std::string path = std::string(kBerryMiscDir) + ".berry-pin.html";
  FILE* f = fopen(path.c_str(), "w");
  if (f) {
    fwrite(h.data(), 1, h.size(), f);
    fclose(f);
  }
  GetUIThreadTaskRunner({})->PostTask(
      FROM_HERE, base::BindOnce(&Shell::LoadURL, base::Unretained(shell),
                                GURL("file://" + path)));
}

void BerryHandlePin(Shell* shell, const GURL& url) {
  base::StringPairs pairs;
  base::SplitStringIntoKeyValuePairs(url.query(), '=', '&', &pairs);
  std::string title, dest;
  for (const auto& p : pairs) {
    std::string val = p.second;
    std::string dec;
    dec.reserve(val.size());
    for (char c : val)
      dec += (c == '+') ? ' ' : c;
    dec = base::UnescapeBinaryURLComponent(dec);
    if (p.first == "title")
      title = dec;
    else if (p.first == "url")
      dest = dec;
  }
  if (!dest.empty() && dest.find("://") == std::string::npos)
    dest = "https://" + dest;
  if (title.empty()) {
    GURL g(dest);
    title = g.is_valid() && !g.host().empty() ? g.host() : "Berry Browser";
  }
  const bool ok = BerryAddHomeScreenShortcut(title, dest);
  BerryShowPinResult(shell, ok, title, dest);
}

// Load the bundled landing page (the quick-links + Settings tile home), used by
// the Settings page's "Home" link. Derives the app's native dir from our own
// executable path, the same way the launcher resolves it.
void BerryShowHome(Shell* shell) {
  char exe[2048];
  exe[0] = '\0';
  int fd = open("/proc/self/exefile", O_RDONLY);
  if (fd >= 0) {
    ssize_t r = read(fd, exe, sizeof(exe) - 1);
    close(fd);
    if (r > 0) {
      exe[r] = '\0';
      while (r > 0 && (exe[r - 1] == '\n' || exe[r - 1] == '\r' ||
                       exe[r - 1] == ' ' || exe[r - 1] == '\0'))
        exe[--r] = '\0';
    }
  }
  std::string home_url = "about:blank";
  if (exe[0]) {
    char* slash = strrchr(exe, '/');
    if (slash) {
      *slash = '\0';
      home_url = std::string("file://") + exe + "/home.html";
    }
  }
  GURL u(home_url);
  GetUIThreadTaskRunner({})->PostTask(
      FROM_HERE, base::BindOnce(&Shell::LoadURL, base::Unretained(shell), u));
}

void BerryShowSettings(Shell* shell) {
  std::string html = BerryBuildSettingsHtml();
  std::string path = std::string(kBerryMiscDir) + ".berry-settings.html";
  FILE* f = fopen(path.c_str(), "w");
  if (f) {
    fwrite(html.data(), 1, html.size(), f);
    fclose(f);
  }
  GURL file_url("file://" + path);
  GetUIThreadTaskRunner({})->PostTask(
      FROM_HERE,
      base::BindOnce(&Shell::LoadURL, base::Unretained(shell), file_url));
}

void BerryHandleSet(Shell* shell, const GURL& url) {
  base::StringPairs pairs;
  base::SplitStringIntoKeyValuePairs(url.query(), '=', '&', &pairs);
  std::string k, v;
  for (const auto& p : pairs) {
    if (p.first == "k")
      k = p.second;
    else if (p.first == "v")
      v = p.second;
  }
  std::string vdec;
  vdec.reserve(v.size());
  for (char c : v)
    vdec += (c == '+') ? ' ' : c;
  vdec = base::UnescapeBinaryURLComponent(vdec);
  const bool on = (vdec == "1");

  if (k == "nojs")
    BerrySetMarker("berry-nojs.enable", on);
  else if (k == "noimages")
    BerrySetMarker("berry-noimages.enable", on);
  else if (k == "dark")
    BerrySetMarker("berry-dark.enable", on);
  else if (k == "adblock")
    BerrySetMarker("berry-adblock.disable", !on);  // on => no disable marker
  else if (k == "logship")
    BerrySetMarker("berry-logship.disable", !on);  // on => no disable marker
  else if (k == "desktop")
    BerrySetMarker("berry-desktop.enable", on);
  else if (k == "lowend") {
    BerrySetMarker("berry-lowend.disable", !on);
    BerrySetMarker("berry-lowend.enable", false);
  } else if (k == "gpu") {
    BerrySetMarker("berry-gpu.disable", !on);
    BerrySetMarker("berry-gpu.enable", false);
  } else if (k == "sw") {
    BerrySetMarker("berry-sw.disable", !on);
    BerrySetMarker("berry-sw.enable", false);
  } else if (k == "quic") {
    BerrySetMarker("berry-quic.enable", on);
    BerrySetMarker("berry-quic.disable", false);
  } else if (k == "http1")
    BerrySetMarker("berry-http1.enable", on);
  else if (k == "altsvc")
    BerrySetMarker("berry-alt-svc.disable", on);
  else if (k == "block") {
    BerrySetMarker("berry-block.disable", !on);
    BerrySetMarker("berry-block.enable", false);
  } else if (k == "ytmobile")
    BerrySetMarker("berry-youtube-mobile.enable", on);
  else if (k == "diskcache")
    BerrySetMarker("berry-disk-cache.disable", !on);
  else if (k == "privacy") {
    BerrySetMarker("berry-privacy.enable", !on);
    BerrySetMarker("berry-privacy.disable", false);
  } else if (k == "wasmliftoff") {
    BerrySetMarker("berry-wasm-liftoff.enable", on);
    BerrySetMarker("berry-wasm-tier-up.enable", false);
  } else if (k == "stalllog")
    BerrySetMarker("berry-stall-log.disable", !on);
  else if (k == "mic")
    BerrySetMarker("berry-mic.enable", on);
  else if (k == "videodebug")
    BerrySetMarker("berry-video.debug", on);
  else if (k == "kbddebug") {
    BerrySetMarker("berry-kbd.debug", on);
    if (!on)
      BerrySetMarker("berry-nav.debug", false);
  } else if (k == "res") {
    BerrySetMarker("berry-x-420.enable", false);
    BerrySetMarker("berry-x-540.enable", false);
    BerrySetMarker("berry-x-720.enable", false);
    BerrySetMarker("berry-x-1440.enable", false);
    if (vdec == "420")
      BerrySetMarker("berry-x-420.enable", true);
    else if (vdec == "540")
      BerrySetMarker("berry-x-540.enable", true);
    else if (vdec == "720")
      BerrySetMarker("berry-x-720.enable", true);
    else if (vdec == "1440")
      BerrySetMarker("berry-x-1440.enable", true);
  } else if (k == "fps") {
    BerrySetMarker("berry-x-perf.enable", false);
    BerrySetMarker("berry-x-lite.enable", false);
    BerrySetMarker("berry-x-fullfps.enable", false);
    BerrySetMarker("berry-x-slow10.enable", false);
    BerrySetMarker("berry-x-slow12.enable", false);
    BerrySetMarker("berry-x-slow15.enable", false);
    BerrySetMarker("berry-x-slow45.enable", false);
    if (vdec == "perf")
      BerrySetMarker("berry-x-perf.enable", true);
    else if (vdec == "lite")
      BerrySetMarker("berry-x-lite.enable", true);
    else if (vdec == "60")
      BerrySetMarker("berry-x-fullfps.enable", true);
    else if (vdec == "12")
      BerrySetMarker("berry-x-slow12.enable", true);
    else if (vdec == "10")
      BerrySetMarker("berry-x-slow10.enable", true);
    else if (vdec == "15")
      BerrySetMarker("berry-x-slow15.enable", true);
    else if (vdec == "45")
      BerrySetMarker("berry-x-slow45.enable", true);
  } else if (k == "ua")
    BerrySetTextMarker("berry-ua", vdec);
  else if (k == "home")
    BerrySetTextMarker("berry-home-url", vdec);
  else if (k == "logshipurl")
    BerrySetTextMarker("berry-logship-url", vdec);
  else if (k == "device") {
    if (vdec == "auto") {
      /* Auto = no marker; the launcher detects the panel via libscreen. */
      BerrySetTextMarker("berry-device", "");
    } else if (vdec == "passport" || vdec == "classic" || vdec == "q20" ||
               vdec == "q10" || vdec == "q5" || vdec == "z10" ||
               vdec == "z30" || vdec == "z3" || vdec == "leap") {
      BerrySetTextMarker("berry-device", vdec);
    }
  }

  BerryShowSettings(shell);
}

std::string g_berry_last_http_url;

bool BerryUrlIsShareable(const GURL& url) {
  return url.is_valid() && url.SchemeIsHTTPOrHTTPS() &&
         url.host() != "berry.share" && url.host() != "berry.pin" &&
         url.host() != "berry.set" && url.host() != "berry.settings" &&
         url.host() != "berry.home" && url.host() != "berry.restart";
}

void BerryNavigatorInvokeUri(const std::string& uri, const char* action) {
  navigator_invoke_invocation_t* inv = nullptr;
  if (navigator_invoke_invocation_create(&inv) != BPS_SUCCESS)
    return;
  if (action && action[0])
    navigator_invoke_invocation_set_action(inv, action);
  navigator_invoke_invocation_set_uri(inv, uri.c_str());
  const int rc = navigator_invoke_invocation_send(inv);
  navigator_invoke_invocation_destroy(inv);
  QNX_NAV_LOG_FMT("BerryNav: invoke uri=\"%s\" action=%s rc=%d\n", uri.c_str(),
                  action ? action : "", rc);
}

void BerryNavigatorShareImpl(const std::string& url) {
  GURL g(url);
  std::string share = url;
  if (!BerryUrlIsShareable(g))
    share = g_berry_last_http_url;
  if (share.empty()) {
    QNX_NAV_LOG_FMT("%s", "BerryNav: share skipped (no url)\n");
    return;
  }
  navigator_invoke_invocation_t* inv = nullptr;
  if (navigator_invoke_invocation_create(&inv) != BPS_SUCCESS)
    return;
  navigator_invoke_invocation_set_action(inv, "bb.action.SHARE");
  navigator_invoke_invocation_set_type(inv, "text/plain");
  navigator_invoke_invocation_set_data(inv, share.data(),
                                       static_cast<int>(share.size()));
  const int rc = navigator_invoke_invocation_send(inv);
  navigator_invoke_invocation_destroy(inv);
  QNX_NAV_LOG_FMT("BerryNav: share rc=%d url=\"%s\"\n", rc, share.c_str());
}

void BerryUpdateWindowCoverImpl(const GURL& url) {
  std::string label;
  if (url.SchemeIsFile() &&
      url.path().find("home.html") != std::string::npos) {
    label = "Berry Browser";
  } else if (url.host() == "berry.settings" ||
             url.path().find(".berry-settings") != std::string::npos) {
    label = "Settings";
  } else if (!url.host().empty()) {
    label = url.host();
    if (label.size() > 4 && label.compare(0, 4, "www.") == 0)
      label = label.substr(4);
  } else {
    label = "Berry Browser";
  }
  if (label.size() > 28)
    label.resize(28);

  navigator_window_cover_attribute_t* attr = nullptr;
  if (navigator_window_cover_attribute_create(&attr) != BPS_SUCCESS)
    return;
  const std::string dir = BerryNativeDir();
  const std::string icon = dir + "/icon.png";
  if (!dir.empty() && access(icon.c_str(), R_OK) == 0)
    navigator_window_cover_attribute_set_file(attr, icon.c_str());
  navigator_window_cover_label_t* lab = nullptr;
  if (navigator_window_cover_attribute_add_label(attr, label.c_str(), &lab) ==
          BPS_SUCCESS &&
      lab) {
    navigator_window_cover_label_set_color(lab, 233, 84, 32);
    navigator_window_cover_label_set_size(lab, 10);
  }
  const int rc = navigator_window_cover_update(attr);
  if (lab)
    navigator_window_cover_label_destroy(lab);
  navigator_window_cover_attribute_destroy(attr);
  QNX_NAV_LOG_FMT("BerryNav: cover rc=%d label=\"%s\"\n", rc, label.c_str());
}

void BerryStayOnCurrentPage(Shell* shell, const GURL& blocked) {
  if (!shell || !shell->web_contents())
    return;
  GURL stay = shell->web_contents()->GetLastCommittedURL();
  if (!stay.is_valid() || stay == blocked || stay.IsAboutBlank()) {
    BerryShowHome(shell);
    return;
  }
  GetUIThreadTaskRunner({})->PostTask(
      FROM_HERE, base::BindOnce(&Shell::LoadURL, base::Unretained(shell), stay));
}

void BerryHandleShare(Shell* shell, const GURL& url) {
  base::StringPairs pairs;
  base::SplitStringIntoKeyValuePairs(url.query(), '=', '&', &pairs);
  std::string dest;
  for (const auto& p : pairs) {
    if (p.first != "url")
      continue;
    std::string dec;
    dec.reserve(p.second.size());
    for (char c : p.second)
      dec += (c == '+') ? ' ' : c;
    dest = base::UnescapeBinaryURLComponent(dec);
  }
  BerryNavigatorShareImpl(dest);
  BerryStayOnCurrentPage(shell, url);
}
}  // namespace

void BerryUpdateWindowCover(const GURL& url) {
  BerryUpdateWindowCoverImpl(url);
}

void BerryNavigatorShare(const std::string& url) {
  BerryNavigatorShareImpl(url);
}
#endif

void Shell::DidStartNavigation(NavigationHandle* navigation_handle) {
#if BUILDFLAG(IS_QNX)
  if (!navigation_handle->IsInPrimaryMainFrame())
    return;
  const GURL& url = navigation_handle->GetURL();
  std::string spec = url.spec();
  if (spec.size() > 120)
    spec = spec.substr(0, 120);
  QNX_NAV_LOG_FMT(
      "BerryNav: DidStartNavigation url=\"%s\" same_doc=%d\n", spec.c_str(),
      navigation_handle->IsSameDocument() ? 1 : 0);
  // In-app "Restart browser" action. Any navigation to the sentinel host
  // triggers a graceful relaunch -- the home.html "Restart" tile links here, and
  // typing "berry.restart" in the URL bar works too. We catch it at navigation
  // *start*, so it never reaches DNS/network. Deferred to a fresh task so the
  // teardown/re-exec doesn't run inside this navigation observer callback. Keep
  // the host in sync with home.html.
  if (!navigation_handle->IsSameDocument() && url.host() == "berry.restart") {
    QNX_NAV_LOG_FMT("%s", "BerryNav: restart sentinel -> graceful relaunch\n");
    GetUIThreadTaskRunner({})->PostTask(FROM_HERE,
                                        base::BindOnce(&BerryRestartRelaunch));
    return;
  }
  // In-app Settings (engine-served, writes marker files in shared/misc).
  if (!navigation_handle->IsSameDocument() && url.host() == "berry.set") {
    QNX_NAV_LOG_FMT("%s", "BerryNav: settings write\n");
    BerryHandleSet(this, url);
    return;
  }
  if (!navigation_handle->IsSameDocument() && url.host() == "berry.settings") {
    QNX_NAV_LOG_FMT("%s", "BerryNav: open settings\n");
    BerryShowSettings(this);
    return;
  }
  if (!navigation_handle->IsSameDocument() && url.host() == "berry.home") {
    QNX_NAV_LOG_FMT("%s", "BerryNav: open home\n");
    BerryShowHome(this);
    return;
  }
  if (!navigation_handle->IsSameDocument() && url.host() == "berry.pin") {
    QNX_NAV_LOG_FMT("%s", "BerryNav: pin to home screen\n");
    BerryHandlePin(this, url);
    return;
  }
  if (!navigation_handle->IsSameDocument() && url.host() == "berry.share") {
    QNX_NAV_LOG_FMT("%s", "BerryNav: share\n");
    BerryHandleShare(this, url);
    return;
  }
  if (!navigation_handle->IsSameDocument() &&
      (url.SchemeIs("mailto") || url.SchemeIs("tel") || url.SchemeIs("sms") ||
       url.SchemeIs("mmsto"))) {
    QNX_NAV_LOG_FMT("BerryNav: handoff \"%s\"\n", url.spec().c_str());
    BerryNavigatorInvokeUri(url.spec(), "bb.action.OPEN");
    BerryStayOnCurrentPage(this, url);
    return;
  }
  if (!navigation_handle->IsSameDocument() && url.scheme() == "intent") {
    const GURL https = BerryIntentUrlToHttps(url);
    QNX_NAV_LOG_FMT("BerryNav: blocked intent:// -> \"%s\"\n",
                    https.is_valid() ? https.spec().substr(0, 100).c_str()
                                     : "(invalid)");
    if (https.is_valid()) {
      Shell* shell = this;
      GetUIThreadTaskRunner({})->PostTask(
          FROM_HERE, base::BindOnce(
                         [](Shell* s, const GURL& u) {
                           if (s)
                             s->LoadURL(u);
                         },
                         shell, https));
    }
    return;
  }
  if (BerryIsGoogleMapsUrl(url)) {
    BerryApplyFixedGeolocationOverride(web_contents_.get());
    BerryScheduleMapsGeolocationRetries(web_contents_.get());
    QNX_NAV_LOG_FMT("%s", "BerryNav: Maps geolocation override applied\n");
    BerryMaybeRedirectBrokenMapsCamera(this, url,
                                       navigation_handle->IsSameDocument());
  }
  // Pin visibility early — YouTube aborts googlevideo fetches if hidden.
  if (!navigation_handle->IsSameDocument() && !navigation_handle->IsErrorPage() &&
      url.SchemeIsHTTPOrHTTPS()) {
    const std::string host = url.host();
    if (host.find("youtube.com") != std::string::npos ||
        host.find("googlevideo.com") != std::string::npos) {
      web_contents_->UpdateWebContentsVisibility(Visibility::VISIBLE);
    }
  }
  // Per-host desktop UA (YouTube/googlevideo, WhatsApp, Google Maps). Do NOT
  // force desktop globally — embedded Maps on third-party sites (e.g.
  // hihostels.ca) needs the mobile UA + viewport so the Maps raster path
  // initializes and fetches khms/vt tiles.
  if (!navigation_handle->IsSameDocument() && url.SchemeIsHTTPOrHTTPS()) {
    const bool want_desktop = BerryHostPrefersDesktopUA(url);
    blink::UserAgentOverride ov;
    if (want_desktop) {
      ov.ua_string_override = GetBerryDesktopUserAgent();
      ov.ua_metadata_override = GetBerryDesktopUserAgentMetadata();
    }
    // Always push override state — empty clears a prior desktop UA when leaving
    // YouTube/WhatsApp/Messenger/Maps for a normal site (embedded Maps needs mobile UA).
    web_contents_->SetUserAgentOverride(ov, /*override_in_new_tabs=*/false);
    navigation_handle->SetIsOverridingUserAgent(want_desktop);
    QNX_NAV_LOG_FMT("BerryNav: UA = %s for host=\"%s\"\n",
                    want_desktop ? "desktop" : "mobile", url.host().c_str());
  }
  // Update the toolbar as soon as navigation starts so the user sees the
  // destination URL while the network fetch runs (commit can take 20+ s).
  if (!navigation_handle->IsSameDocument()) {
    g_platform->SetAddressBarURL(this, url);
    g_platform->SetIsLoading(this, true);
  }
#endif
}

void Shell::DidFinishNavigation(NavigationHandle* navigation_handle) {
#if BUILDFLAG(IS_QNX)
  if (navigation_handle->IsInPrimaryMainFrame()) {
    const GURL& url = navigation_handle->GetURL();
    std::string spec = url.spec();
    if (spec.size() > 120)
      spec = spec.substr(0, 120);
    const int64_t ms =
        (base::TimeTicks::Now() - navigation_handle->NavigationStart())
            .InMilliseconds();
    QNX_NAV_LOG_FMT(
        "BerryNav: DidFinishNavigation url=\"%s\" error=%d code=%d ms=%lld\n",
        spec.c_str(), navigation_handle->IsErrorPage() ? 1 : 0,
        static_cast<int>(navigation_handle->GetNetErrorCode()),
        static_cast<long long>(ms));
    // Navigator can report hidden after first paint; mobile/desktop YouTube
    // players abort googlevideo fetches when visibility != visible.
    if (!navigation_handle->IsErrorPage() && url.SchemeIsHTTPOrHTTPS()) {
      const std::string host = url.host();
      if (host.find("youtube.com") != std::string::npos ||
          host.find("googlevideo.com") != std::string::npos ||
          BerryIsGoogleMapsUrl(url)) {
        web_contents_->UpdateWebContentsVisibility(Visibility::VISIBLE);
      }
    }
    if (!navigation_handle->IsErrorPage() && BerryIsGoogleMapsUrl(url)) {
      BerryApplyFixedGeolocationOverride(web_contents_.get());
      BerryScheduleMapsGeolocationRetries(web_contents_.get());
      QNX_NAV_LOG_FMT("%s", "BerryNav: Maps geolocation re-applied post-nav\n");
    }
    if (!navigation_handle->IsErrorPage() && url.SchemeIsHTTPOrHTTPS() &&
        BerryUrlIsShareable(url))
      g_berry_last_http_url = url.spec();
  }
#endif
  MaybeArmDumpTimeout(navigation_handle);
}

void Shell::DOMContentLoaded(RenderFrameHost* render_frame_host) {
  if (!render_frame_host->IsInPrimaryMainFrame())
    return;
  auto* cmd = base::CommandLine::ForCurrentProcess();
  if (!cmd->HasSwitch(switches::kDumpDom))
    return;
  std::string trigger = cmd->GetSwitchValueASCII(switches::kDomTrigger);
  if (trigger != "domcontentloaded")
    return;
  QNX_TRACE_MSG("QNX:Shell:DOMContentLoaded trigger\n");
  DumpDomAndExit(render_frame_host);
}

void Shell::DidFinishLoad(RenderFrameHost* render_frame_host,
                          const GURL& validated_url) {
  QNX_TRACE_FMT("QNX:Shell:DFL enter main=%d dump=%d\n",
                (int)render_frame_host->IsInPrimaryMainFrame(),
                (int)base::CommandLine::ForCurrentProcess()->HasSwitch(switches::kDumpDom));
  if (!render_frame_host->IsInPrimaryMainFrame())
    return;
  if (!base::CommandLine::ForCurrentProcess()->HasSwitch(switches::kDumpDom))
    return;
  if (base::QnxBerryDaemonEnabled())
    return;
  QNX_TRACE_MSG("QNX:Shell:DFL exec\n");
  DumpDomAndExit(render_frame_host);
}

JavaScriptDialogManager* Shell::GetJavaScriptDialogManager(
    WebContents* source) {
  if (!dialog_manager_)
    dialog_manager_ = g_platform->CreateJavaScriptDialogManager(this);
  if (!dialog_manager_)
    dialog_manager_ = std::make_unique<ShellJavaScriptDialogManager>();
  return dialog_manager_.get();
}

#if BUILDFLAG(IS_MAC)
void Shell::PrimaryPageChanged(Page& page) {
  g_platform->DidNavigatePrimaryMainFramePostCommit(
      this, WebContents::FromRenderFrameHost(&page.GetMainDocument()));
}

bool Shell::HandleKeyboardEvent(WebContents* source,
                                const NativeWebKeyboardEvent& event) {
  return g_platform->HandleKeyboardEvent(this, source, event);
}
#endif

bool Shell::DidAddMessageToConsole(WebContents* source,
                                   blink::mojom::ConsoleMessageLevel log_level,
                                   const std::u16string& message,
                                   int32_t line_no,
                                   const std::u16string& source_id) {
  return switches::IsRunWebTestsSwitchPresent();
}

void Shell::PortalWebContentsCreated(WebContents* portal_web_contents) {
  g_platform->DidCreateOrAttachWebContents(this, portal_web_contents);
}

void Shell::RendererUnresponsive(
    WebContents* source,
    RenderWidgetHost* render_widget_host,
    base::RepeatingClosure hang_monitor_restarter) {
  LOG(WARNING) << "renderer unresponsive";
}

void Shell::ActivateContents(WebContents* contents) {
#if !BUILDFLAG(IS_MAC)
  // TODO(danakj): Move this to ShellPlatformDelegate.
  contents->Focus();
#else
  // Mac headless mode is quite different than other platforms. Normally
  // focusing the WebContents would cause the OS to focus the window. Because
  // headless mac doesn't actually have system windows, we can't go down the
  // normal path and have to fake it out in the browser process.
  g_platform->ActivateContents(this, contents);
#endif
}

#if BUILDFLAG(IS_ANDROID) || BUILDFLAG(IS_APPLE)
std::unique_ptr<ColorChooser> Shell::OpenColorChooser(
    WebContents* web_contents,
    SkColor color,
    const std::vector<blink::mojom::ColorSuggestionPtr>& suggestions) {
  return g_platform->OpenColorChooser(web_contents, color, suggestions);
}
#endif

void Shell::RunFileChooser(RenderFrameHost* render_frame_host,
                           scoped_refptr<FileSelectListener> listener,
                           const blink::mojom::FileChooserParams& params) {
  run_file_chooser_count_++;
  if (hold_file_chooser_) {
    held_file_chooser_listener_ = std::move(listener);
  } else {
    g_platform->RunFileChooser(render_frame_host, std::move(listener), params);
  }
}

void Shell::EnumerateDirectory(WebContents* web_contents,
                               scoped_refptr<FileSelectListener> listener,
                               const base::FilePath& path) {
  run_file_chooser_count_++;
  if (hold_file_chooser_) {
    held_file_chooser_listener_ = std::move(listener);
  } else {
    listener->FileSelectionCanceled();
  }
}

bool Shell::IsBackForwardCacheSupported() {
  return true;
}

PreloadingEligibility Shell::IsPrerender2Supported(WebContents& web_contents) {
  return PreloadingEligibility::kEligible;
}

std::unique_ptr<WebContents> Shell::ActivatePortalWebContents(
    WebContents* predecessor_contents,
    std::unique_ptr<WebContents> portal_contents) {
  DCHECK_EQ(predecessor_contents, web_contents_.get());
  portal_contents->SetDelegate(this);
  web_contents_->SetDelegate(nullptr);
  std::swap(web_contents_, portal_contents);
  g_platform->SetContents(this);
  g_platform->SetAddressBarURL(this, web_contents_->GetVisibleURL());
  LoadingStateChanged(web_contents_.get(), true);
  return portal_contents;
}

namespace {
class PendingCallback : public base::RefCounted<PendingCallback> {
 public:
  explicit PendingCallback(base::OnceCallback<void()> cb)
      : callback_(std::move(cb)) {}

 private:
  friend class base::RefCounted<PendingCallback>;
  ~PendingCallback() { std::move(callback_).Run(); }
  base::OnceCallback<void()> callback_;
};
}  // namespace

void Shell::UpdateInspectedWebContentsIfNecessary(
    WebContents* old_contents,
    WebContents* new_contents,
    base::OnceCallback<void()> callback) {
  scoped_refptr<PendingCallback> pending_callback =
      base::MakeRefCounted<PendingCallback>(std::move(callback));
  for (auto* shell_devtools_bindings :
       ShellDevToolsBindings::GetInstancesForWebContents(old_contents)) {
    shell_devtools_bindings->UpdateInspectedWebContents(
        new_contents, base::DoNothingWithBoundArgs(pending_callback));
  }
}

bool Shell::ShouldAllowRunningInsecureContent(WebContents* web_contents,
                                              bool allowed_per_prefs,
                                              const url::Origin& origin,
                                              const GURL& resource_url) {
  if (allowed_per_prefs)
    return true;

  return g_platform->ShouldAllowRunningInsecureContent(this);
}

PictureInPictureResult Shell::EnterPictureInPicture(WebContents* web_contents) {
  // During tests, returning success to pretend the window was created and allow
  // tests to run accordingly.
  if (!switches::IsRunWebTestsSwitchPresent())
    return PictureInPictureResult::kNotSupported;
  return PictureInPictureResult::kSuccess;
}

bool Shell::ShouldResumeRequestsForCreatedWindow() {
  return !delay_popup_contents_delegate_for_testing_;
}

void Shell::SetContentsBounds(WebContents* source, const gfx::Rect& bounds) {
  DCHECK(source == web_contents());  // There's only one WebContents per Shell.

  if (switches::IsRunWebTestsSwitchPresent()) {
    // Note that chrome drops these requests on normal windows.
    // TODO(danakj): The position is dropped here but we use the size. Web tests
    // can't move the window in headless mode anyways, but maybe we should be
    // letting them pretend?
    g_platform->ResizeWebContent(this, bounds.size());
  }
}

gfx::Size Shell::GetShellDefaultSize() {
  static gfx::Size default_shell_size;  // Only go through this method once.

  if (!default_shell_size.IsEmpty())
    return default_shell_size;

  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  if (command_line->HasSwitch(switches::kContentShellHostWindowSize)) {
    const std::string size_str = command_line->GetSwitchValueASCII(
        switches::kContentShellHostWindowSize);
    int width, height;
    if (sscanf(size_str.c_str(), "%dx%d", &width, &height) == 2) {
      default_shell_size = gfx::Size(width, height);
    } else {
      LOG(ERROR) << "Invalid size \"" << size_str << "\" given to --"
                 << switches::kContentShellHostWindowSize;
    }
  }

  if (default_shell_size.IsEmpty()) {
    default_shell_size =
        gfx::Size(kDefaultTestWindowWidthDip, kDefaultTestWindowHeightDip);
  }

  return default_shell_size;
}

#if BUILDFLAG(IS_ANDROID)
void Shell::LoadProgressChanged(double progress) {
  g_platform->LoadProgressChanged(this, progress);
}
#endif

void Shell::TitleWasSet(NavigationEntry* entry) {
  if (entry)
    g_platform->SetTitle(this, entry->GetTitle());
}

}  // namespace content
