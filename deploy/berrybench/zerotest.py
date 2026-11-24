#!/usr/bin/env python3
"""Are freshly created ArrayBuffers zero-filled? zerotest.py <engine>

Fills many buffers with a marker, drops them, collects garbage so their
memory goes back to the allocator, then creates new buffers and looks for the
marker. A correct engine never shows it.
"""
import json, os, sys
from engine import Engine, stop_all
JS = r"""
(async function () {
  const out = {};
  for (const size of [4096, 65536, 262144]) {
    const n = Math.min(600, Math.floor(48 * 1024 * 1024 / size));
    let keep = [];
    for (let i = 0; i < n; i++) { const u = new Uint8Array(size); u.fill(0xAB); keep.push(u); }
    keep = null;
    for (let k = 0; k < 4; k++) { gc(); await new Promise(r => setTimeout(r, 300)); }
    let dirty = 0, fresh = [];
    for (let i = 0; i < n; i++) {
      const u = new Uint8Array(size);
      let bad = false;
      for (let j = 0; j < size; j += 1024) if (u[j] !== 0) { bad = true; break; }
      if (bad) dirty++;
      fresh.push(u);
    }
    out[size] = dirty + "/" + n;
    fresh = null;
    for (let k = 0; k < 2; k++) { gc(); await new Promise(r => setTimeout(r, 200)); }
  }
  return JSON.stringify(out);
})()
"""
stop_all()
with Engine(sys.argv[1], jsflags="--max-old-space-size=256 --no-optimize-for-size --expose-gc",
            log_path="logs/zerotest-%s.log" % sys.argv[1].replace("/", "_"),
            env=os.environ.get("ENGINE_ENV", ""),
            extra="--content-shell-host-window-size=540x540") as e:
    e.start()
    r = e.cdp.evaluate(JS, await_promise=True, timeout=300)
    print("%s: new buffers that were not zero-filled, by size: %s" % (sys.argv[1], r))
