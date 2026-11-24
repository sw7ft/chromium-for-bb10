#!/usr/bin/env python3
"""Profile one page load on the phone bench.

usage: pageprof.py <label> <engine> <url> [cold|warm] [settle_s]

  cold  wipe the bench profile first (no HTTP cache, no code cache)
  warm  keep the profile from the previous run (repeat visit)

environment:
  TRACE=0        skip the Chrome trace (less overhead; CPU and state data only)
  SAMPLE=0       skip the thread-state sampler (it loads the process manager
                 and so slows the engine's own memory calls; use for timing)
  JSFLAGS=...    V8 flags (default: what the phone runs)
  ENGINE_ENV=... environment for the engine, e.g. "MALLOC_MEMORY_HOLD=1"
  EXTRA=...      extra engine switches

Three kinds of data go into results/prof-<label>.json:
  * a Chrome trace of the load (loadable in chrome://tracing or Perfetto),
  * CPU seconds per engine thread, per process and for the QNX process
    manager, read with pidin before and after,
  * thread-state samples (results/prof-<label>.samples, about 60 Hz): whether
    each engine thread was running or what it was blocked on.
Summarise with profsum.py (trace) and ttsum.py (CPU and states). Only the
bench engine and its private profile are touched.
"""
import json
import os
import subprocess
import sys
import time

from engine import BENCH, PHONE, Engine, phone, stop_all, wipe_profile

label, engine, url = sys.argv[1], sys.argv[2], sys.argv[3]
mode = sys.argv[4] if len(sys.argv) > 4 else "cold"
settle = float(sys.argv[5]) if len(sys.argv) > 5 else 8

TRACE = os.environ.get("TRACE", "1") == "1"
SAMPLE = os.environ.get("SAMPLE", "1") == "1"
JSFLAGS = os.environ.get("JSFLAGS", "--max-old-space-size=256 --no-optimize-for-size")
CATS = [
    "toplevel", "devtools.timeline", "disabled-by-default-devtools.timeline",
    "v8", "v8.execute", "disabled-by-default-v8.compile", "blink",
    "blink.user_timing", "loading", "navigation", "__metadata",
]
name = engine.split("/")[-1]
flag = BENCH + "/data/sampling"


def cpu_times():
    """CPU seconds: engine threads, all processes, process-manager threads."""
    return phone("pidin -P %s ttimes; echo ==ALL==; pidin times; echo ==PROCNTO==; "
                 "pidin -p 1 ttimes" % name).stdout


stop_all()
if mode == "cold":
    wipe_profile()

os.makedirs("results", exist_ok=True)
events, finished, lost, trace_error = [], False, None, None
with Engine(engine, jsflags=JSFLAGS, log_path="logs/prof-%s.log" % label,
            env=os.environ.get("ENGINE_ENV", ""),
            extra="--content-shell-host-window-size=540x540 " +
                  os.environ.get("EXTRA", "")) as e:
    e.start()
    c = e.cdp
    if TRACE:
        c.call("Tracing.start", {
            "transferMode": "ReportEvents",
            "traceConfig": {"recordMode": "recordUntilFull",
                            "traceBufferSizeInKb": 300 * 1024,
                            "includedCategories": CATS,
                            "excludedCategories": ["*"]}})
    pid = phone("pidin -F '%%a %%N' | grep %s" % name).stdout.split()[0]
    phone("echo 1 > %s" % flag)
    # Bounded loop that also stops when the flag file goes away.
    sampler = subprocess.Popen(
        ["ssh", "-o", "BatchMode=yes", PHONE,
         "i=0; while [ -e %s ] && [ $i -lt %d ]; do pidin -p %s -F '%%b %%J %%B'; echo ==; "
         "i=$((i+1)); done" % (flag, 6000 if SAMPLE else 0, pid)],
        stdout=open("results/prof-%s.samples" % label, "w"), stderr=subprocess.DEVNULL,
        stdin=subprocess.DEVNULL)
    try:
        cpu0 = cpu_times()
        t0 = time.time()
        c.call("Page.navigate", {"url": url}, timeout=120)
        done_at = None
        state = {}
        while time.time() - t0 < 150:
            c.pump(1)
            try:
                state = json.loads(c.evaluate(
                    "JSON.stringify({rs: document.readyState, title: document.title,"
                    " url: location.href, n: document.getElementsByTagName('*').length})",
                    timeout=90))
            except Exception as ex:  # page busy or navigating
                state = {"err": str(ex)[:100]}
                continue
            if state.get("rs") == "complete":
                done_at = done_at or time.time()
                if time.time() - done_at >= settle:
                    break
        wall = time.time() - t0
        cpu1 = cpu_times()
    finally:
        phone("rm -f %s" % flag)
        sampler.wait(timeout=30)
    if TRACE:
        try:
            c.take_events()
            c.call("Tracing.end")
            end = time.time() + 600
            while time.time() < end and not finished:
                c.pump(2)
                for m in c.take_events():
                    if m.get("method") == "Tracing.dataCollected":
                        events += m["params"]["value"]
                    elif m.get("method") == "Tracing.tracingComplete":
                        finished = True
                        lost = m["params"].get("dataLossOccurred")
        except Exception as ex:  # the DevTools socket sometimes drops here
            trace_error = str(ex)[:200]

json.dump({"traceEvents": events,
           "berry": {"label": label, "engine": engine, "url": url, "mode": mode,
                     "jsflags": JSFLAGS, "engine_env": os.environ.get("ENGINE_ENV", ""),
                     "wall": wall,
                     "load_complete_s": (done_at - t0) if done_at else None,
                     "state": state, "trace": TRACE, "complete": finished,
                     "data_loss": lost, "trace_error": trace_error,
                     "ttimes": [cpu0, cpu1]}},
          open("results/prof-%s.json" % label, "w"))
print("%s: %d events, load complete %.1fs, measured %.1fs, title=%r n=%s%s" % (
    label, len(events), (done_at - t0) if done_at else -1, wall,
    state.get("title", "")[:50], state.get("n"),
    (" TRACE LOST: " + trace_error) if trace_error else ""))
