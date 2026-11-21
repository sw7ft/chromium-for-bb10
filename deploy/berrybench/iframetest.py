#!/usr/bin/env python3
"""iframe load-event ordering checks: iframetest.py <engine> [jsflags|-]

Each case records every load event an <iframe> element receives, together
with the URL its document had at that moment. A case passes when the handler
only ever sees the document it asked for.
"""
import json, sys
from engine import Engine, stop_all

PAGE = "https://browserbench.org/Speedometer3.0/resources/todomvc/architecture-examples/backbone/dist/index.html"

JS = r"""(async function(){
  var real = location.href;
  function href(f){ try { return f.contentWindow.location.href; } catch (e) { return 'ERR ' + e; } }
  function run(setup){
    return new Promise(function(resolve){
      var f = document.createElement('iframe');
      var seen = [];
      var inSetup = true;
      var h = function(){ seen.push((inSetup ? 'sync:' : 'async:') + href(f)); };
      setup(f, h);
      inSetup = false;
      setTimeout(function(){ f.remove(); resolve(seen); }, 6000);
    });
  }
  var out = {};
  // Speedometer's order: insert, then attach the handler and point it somewhere.
  out.insert_then_onload_src = await run(function(f, h){ document.body.appendChild(f); f.onload = h; f.src = real; });
  out.insert_then_listener_src = await run(function(f, h){ document.body.appendChild(f); f.addEventListener('load', h); f.src = real; });
  out.insert_then_src_onload = await run(function(f, h){ document.body.appendChild(f); f.src = real; f.onload = h; });
  // Handler and src before insertion: one event, for the real document.
  out.onload_src_then_insert = await run(function(f, h){ f.onload = h; f.src = real; document.body.appendChild(f); });
  // No navigation at all: the blank document's own load event must still arrive.
  out.onload_then_insert_blank = await run(function(f, h){ f.onload = h; document.body.appendChild(f); });
  // srcdoc.
  out.onload_srcdoc_then_insert = await run(function(f, h){ f.onload = h; f.srcdoc = '<p>hi</p>'; document.body.appendChild(f); });
  // Navigate an existing frame a second time.
  out.renavigate = await new Promise(function(resolve){
    var f = document.createElement('iframe'); var seen = [];
    f.onload = function(){ seen.push(href(f)); if (seen.length === 1) f.src = real + '?second'; };
    f.src = real; document.body.appendChild(f);
    setTimeout(function(){ f.remove(); resolve(seen); }, 9000);
  });
  return JSON.stringify({real: real, out: out});
})()"""

eng = sys.argv[1]
jsf = sys.argv[2] if len(sys.argv) > 2 and sys.argv[2] != "-" else None
stop_all()
with Engine(eng, jsflags=jsf, log_path="logs/iframetest-%s.log" % eng.replace("/", "_"),
            extra="--content-shell-host-window-size=540x540") as e:
    e.start()
    c = e.cdp
    c.call("Page.navigate", {"url": PAGE})
    c.pump(8)
    r = json.loads(c.evaluate(JS, await_promise=True, timeout=120))
real = r["real"]
want = {
    "insert_then_onload_src": [real],
    "insert_then_listener_src": [real],
    "insert_then_src_onload": [real],
    "onload_src_then_insert": [real],
    "onload_then_insert_blank": ["about:blank"],
    "onload_srcdoc_then_insert": ["about:srcdoc"],
    "renavigate": [real, real + "?second"],
}
bad = 0
for k, exp in want.items():
    got = r["out"][k]
    urls = [g.split(":", 1)[1] if g.startswith(("sync:", "async:")) else g for g in got]
    ok = urls == exp
    bad += not ok
    short = [g.replace(real, "<page>") for g in got]
    print("%-4s %-28s %s" % ("ok" if ok else "FAIL", k, short))
print("%s: iframe load ordering %d/%d passed" % (eng, len(want) - bad, len(want)))
sys.exit(1 if bad else 0)
