#!/usr/bin/env python3
"""Run Speedometer 3.0 on one bench engine and report whether it completes.

usage: sp3_probe.py <engine> <jsflags|-> [iterations] [suites] [seconds]
"""
import json
import sys
import time

from engine import Engine, console_text, stop_all

engine = sys.argv[1]
jsflags = None if sys.argv[2] == "-" else sys.argv[2]
iters = int(sys.argv[3]) if len(sys.argv) > 3 else 1
suites = sys.argv[4] if len(sys.argv) > 4 else ""
limit = int(sys.argv[5]) if len(sys.argv) > 5 else 600

url = "https://browserbench.org/Speedometer3.0/?iterationCount=%d&startAutomatically=true" % iters
if suites:
    url += "&suites=" + suites

import os
SHIM = os.environ.get("SHIM", "1") == "1"
# Work around a browser bug found on 2026-11-20 (present in build 109 too):
# the load event for an iframe's initial about:blank document is delivered
# after insertion returns instead of during it. Speedometer inserts the frame,
# then assigns onload and src, so the late event starts the suite before the
# real document has loaded. Ignore a load event whose document is still
# about:blank while the element already points at a real URL.
SHIM_JS = """(function(){
  var d = Object.getOwnPropertyDescriptor(HTMLElement.prototype, 'onload');
  if (!d || !d.set) return;
  Object.defineProperty(HTMLIFrameElement.prototype, 'onload', {
    configurable: true, enumerable: true,
    get: function(){ return this.__berryOnload || null; },
    set: function(h){
      this.__berryOnload = h;
      var self = this;
      d.set.call(this, typeof h !== 'function' ? h : function(ev){
        var src = self.getAttribute('src');
        var href = '';
        try { href = self.contentWindow.location.href; } catch (e) {}
        if (src && src !== 'about:blank' && href === 'about:blank') return;
        return h.call(self, ev);
      });
    }
  });
})();"""

stop_all()
tag = "%s-%s" % (engine, "flag" if jsflags else "default")
with Engine(engine, jsflags=jsflags, log_path="logs/sp3-%s-%d.log" % (tag, int(time.time())),
            extra="--content-shell-host-window-size=540x540") as e:
    e.start()
    c = e.cdp
    if SHIM:
        c.call("Page.addScriptToEvaluateOnNewDocument", {"source": SHIM_JS})
    t0 = time.time()
    c.call("Page.navigate", {"url": url})
    status, lines = "timeout", []
    while time.time() - t0 < limit:
        c.pump(5)
        new = console_text(c.take_events())
        for l in new:
            print("  [%5.1fs] %s" % (time.time() - t0, l[:300]), flush=True)
        lines += new
        try:
            st = c.evaluate(
                "JSON.stringify({hash: location.hash, "
                "score: (document.querySelector('#result-number')||{}).textContent||'', "
                "info: (document.querySelector('#info-label')||{}).textContent||''})")
        except Exception as ex:
            print("  evaluate failed:", ex)
            continue
        st = json.loads(st)
        if any(l.startswith("EXCEPTION") for l in lines):
            status = "exception"
            break
        if st["hash"] == "#summary" and st["score"].strip():
            status = "done"
            break
    elapsed = time.time() - t0
    detail = ""
    if status == "done":
        detail = c.evaluate(
            "JSON.stringify(benchmarkClient._measuredValuesList.map("
            "v => ({total: v.total, score: v.score, geomean: v.geomean})))")
    print("RESULT engine=%s jsflags=%s status=%s elapsed=%.0fs state=%s %s" % (
        engine, jsflags or "default", status, elapsed, json.dumps(st), detail))
