#!/usr/bin/env python3
"""Capture a QNX kernel event trace during a page load on the phone bench.

usage: kevcap.py <label> <engine> <url> [start_s] [seconds]

Starts the engine, navigates, and `start_s` seconds later runs the phone's
tracelogger for `seconds`. The trace lands in results/kev-<label>.kev (decode
with the NDK's traceprinter; see kevsum.py). The trace is system-wide but is
written to RAM on the phone, copied here and deleted there straight away.
"""
import json, os, subprocess, sys, time
from engine import PHONE, Engine, phone, stop_all

label, engine, url = sys.argv[1], sys.argv[2], sys.argv[3]
start = float(sys.argv[4]) if len(sys.argv) > 4 else 3
secs = int(sys.argv[5]) if len(sys.argv) > 5 else 3
name = engine.split("/")[-1]
KEV = "/dev/shmem/berry-%s.kev" % label

stop_all()
with Engine(engine, jsflags=os.environ.get("JSFLAGS", "--max-old-space-size=256 --no-optimize-for-size"),
            log_path="logs/kev-%s.log" % label, env=os.environ.get("ENGINE_ENV", ""),
            extra="--content-shell-host-window-size=540x540") as e:
    e.start()
    pid = phone("pidin -F '%%a %%N' | grep %s" % name).stdout.split()[0]
    t0 = time.time()
    e.cdp.call("Page.navigate", {"url": url}, timeout=120)
    time.sleep(max(0, start - (time.time() - t0)))
    t1 = time.time()
    r = phone("tracelogger -w -s %d -f %s 2>&1; ls -la %s" % (secs, KEV, KEV), timeout=120)
    print("tracelogger %.1fs..%.1fs after navigate: %s" % (t1 - t0, time.time() - t0, r.stdout.strip()[-200:]))
subprocess.run(["scp", "-q", "%s:%s" % (PHONE, KEV), "results/kev-%s.kev" % label], check=True)
phone("rm -f %s" % KEV)
json.dump({"pid": int(pid), "start": t1 - t0, "secs": secs, "url": url}, open("results/kev-%s.json" % label, "w"))
print("pid", pid, os.path.getsize("results/kev-%s.kev" % label), "bytes")
