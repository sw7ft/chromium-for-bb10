// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/shell/renderer/shell_render_frame_observer.h"

#include "base/command_line.h"
#include "build/build_config.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_frame_observer.h"
#include "content/shell/common/render_frame_test_helper.mojom.h"
#include "content/shell/common/shell_switches.h"
#include "content/shell/renderer/render_frame_test_helper.h"
#include "third_party/blink/public/web/web_testing_support.h"

#if BUILDFLAG(IS_QNX)
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_script_source.h"
#endif

namespace content {

#if BUILDFLAG(IS_QNX)
namespace {

// JS executed in the page's main world at window-clear time. It only *adds*
// the window.chrome global (purely additive, mirrors what real Chrome exposes
// on every page) and shadows the empty navigator.plugins/pdfViewerEnabled with
// a plausible internal-PDF-viewer set. Everything is wrapped in try/catch so a
// failure can never break page script execution.
const char kBerryChromeShimJs[] = R"JS(
(function() {
  try {
    // Real (non-automated) Chrome reports navigator.webdriver === false. The
    // launcher's --remote-debugging-port flips this to true, which is a hard
    // bot signal for Google's secure-browser / reCAPTCHA checks. Force false.
    Object.defineProperty(window.navigator, 'webdriver',
        { get: function() { return false; }, configurable: true });
  } catch (e) {}

  try {
    var w = window;
    if (!w.chrome) { w.chrome = {}; }
    var c = w.chrome;
    if (!c.runtime) {
      c.runtime = {
        connect: function() {
          return {
            onMessage: { addListener: function() {}, removeListener: function() {} },
            postMessage: function() {},
            disconnect: function() {}
          };
        },
        sendMessage: function() {},
        onMessage: { addListener: function() {}, removeListener: function() {} },
        id: undefined
      };
    }
    if (!c.loadTimes) {
      c.loadTimes = function() {
        var now = (w.performance && performance.now) ? performance.now() / 1000 : 0;
        return {
          requestTime: now, startLoadTime: now, commitLoadTime: now,
          finishDocumentLoadTime: now, finishLoadTime: now,
          firstPaintTime: now, firstPaintAfterLoadTime: 0,
          navigationType: 'Other', wasFetchedViaSpdy: true,
          wasNpnNegotiated: true, npnNegotiatedProtocol: 'h2',
          wasAlternateProtocolAvailable: false, connectionInfo: 'h2'
        };
      };
    }
    if (!c.csi) {
      c.csi = function() {
        return {
          startE: Date.now(), onloadT: Date.now(),
          pageT: (w.performance && performance.now) ? performance.now() : 0,
          tran: 15
        };
      };
    }
    if (!c.app) {
      c.app = {
        isInstalled: false,
        InstallState: { DISABLED: 'disabled', INSTALLED: 'installed', NOT_INSTALLED: 'not_installed' },
        RunningState: { CANNOT_RUN: 'cannot_run', READY_TO_RUN: 'ready_to_run', RUNNING: 'running' },
        getDetails: function() { return null; },
        getIsInstalled: function() { return false; }
      };
    }
  } catch (e) {}

  try {
    var nav = window.navigator;
    var names = [
      ['PDF Viewer', 'internal-pdf-viewer'],
      ['Chrome PDF Viewer', 'internal-pdf-viewer'],
      ['Chromium PDF Viewer', 'internal-pdf-viewer'],
      ['Microsoft Edge PDF Viewer', 'internal-pdf-viewer'],
      ['WebKit built-in PDF', 'internal-pdf-viewer']
    ];
    var mimeTypes = [
      { type: 'application/pdf', suffixes: 'pdf', description: 'Portable Document Format' },
      { type: 'text/pdf', suffixes: 'pdf', description: 'Portable Document Format' }
    ];
    var plugins = [];
    for (var i = 0; i < names.length; i++) {
      var plugin = {
        name: names[i][0],
        filename: names[i][1],
        description: 'Portable Document Format',
        length: 1,
        item: function() { return mimeTypes[0]; },
        namedItem: function() { return mimeTypes[0]; }
      };
      plugin[0] = mimeTypes[0];
      plugins.push(plugin);
    }
    plugins.item = function(i) { return this[i] || null; };
    plugins.namedItem = function(n) {
      for (var i = 0; i < this.length; i++) { if (this[i].name === n) return this[i]; }
      return null;
    };
    plugins.refresh = function() {};
    // Link each mimeType back to its enabling plugin, as Chrome does.
    for (var j = 0; j < mimeTypes.length; j++) { mimeTypes[j].enabledPlugin = plugins[0]; }
    mimeTypes.item = function(i) { return this[i] || null; };
    mimeTypes.namedItem = function(t) {
      for (var i = 0; i < this.length; i++) { if (this[i].type === t) return this[i]; }
      return null;
    };
    Object.defineProperty(nav, 'plugins', { get: function() { return plugins; }, configurable: true });
    Object.defineProperty(nav, 'mimeTypes', { get: function() { return mimeTypes; }, configurable: true });
    Object.defineProperty(nav, 'pdfViewerEnabled', { get: function() { return true; }, configurable: true });
  } catch (e) {}

  try {
    // navigator.platform must agree with the User-Agent or it's a fingerprint
    // inconsistency. The UA is chosen per-navigation in the browser (mobile by
    // default, desktop for hosts like WhatsApp), so derive platform from the
    // effective UA: Android => "Linux armv8l", otherwise "Linux x86_64". The raw
    // QNX value ("Linux armv7l") never leaks either way.
    Object.defineProperty(window.navigator, 'platform', {
      get: function() {
        return (window.navigator.userAgent.indexOf('Android') !== -1)
            ? 'Linux armv8l' : 'Linux x86_64';
      },
      configurable: true
    });
  } catch (e) {}
})();
)JS";

}  // namespace
#endif  // BUILDFLAG(IS_QNX)

ShellRenderFrameObserver::ShellRenderFrameObserver(RenderFrame* render_frame)
    : RenderFrameObserver(render_frame) {}

ShellRenderFrameObserver::~ShellRenderFrameObserver() = default;

void ShellRenderFrameObserver::OnDestruct() {
  delete this;
}

void ShellRenderFrameObserver::DidClearWindowObject() {
  auto& cmd = *base::CommandLine::ForCurrentProcess();
  if (cmd.HasSwitch(switches::kExposeInternalsForTesting)) {
    blink::WebTestingSupport::InjectInternalsObject(
        render_frame()->GetWebFrame());
  }
#if BUILDFLAG(IS_QNX)
  InjectBerryChromeShim();
#endif
}

#if BUILDFLAG(IS_QNX)
void ShellRenderFrameObserver::InjectBerryChromeShim() {
  blink::WebLocalFrame* frame = render_frame()->GetWebFrame();
  if (!frame) {
    return;
  }
  frame->ExecuteScript(
      blink::WebScriptSource(blink::WebString::FromUTF8(kBerryChromeShimJs)));
}
#endif

void ShellRenderFrameObserver::OnInterfaceRequestForFrame(
    const std::string& interface_name,
    mojo::ScopedMessagePipeHandle* interface_pipe) {
  if (interface_name == mojom::RenderFrameTestHelper::Name_) {
    RenderFrameTestHelper::Create(
        *render_frame(), mojo::PendingReceiver<mojom::RenderFrameTestHelper>(
                             std::move(*interface_pipe)));
    return;
  }
}

}  // namespace content
