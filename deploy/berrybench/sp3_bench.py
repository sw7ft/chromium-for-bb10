#!/usr/bin/env python3
"""Speedometer 3.0 A/B on the phone bench.

usage: sp3_bench.py <label> <rounds> <iterations> <suites|all> <cfg> [<cfg> ...]
  cfg = <engine>:<name>:<jsflags or ->        e.g. cs-o0:old:--max-old-space-size=256

Configurations are run interleaved (A B A B ...) so thermal drift and
background activity on the phone hit all of them equally. Each run is a fresh
engine process with a wiped profile. Results append to results/<label>.jsonl.
"""
import json
import os
import statistics
import sys
import time

from engine import Engine, console_text, stop_all, wipe_profile
from sp3_probe_shim import SHIM_JS

label, rounds, iters, suites = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
cfgs = []
for c in sys.argv[5:]:
    eng, name, jsf = c.split(":", 2)
    cfgs.append((eng, name, None if jsf == "-" else jsf))

url = "https://browserbench.org/Speedometer3.0/?iterationCount=%d&startAutomatically=true" % iters
if suites != "all":
    url += "&suites=" + suites

os.makedirs("results", exist_ok=True)
out = open("results/%s.jsonl" % label, "a")


def run_once(eng, name, jsf, rnd):
    stop_all()
    wipe_profile()
    with Engine(eng, jsflags=jsf, log_path="logs/%s-%s-r%d.log" % (label, name, rnd),
                extra="--content-shell-host-window-size=540x540") as e:
        e.start()
        c = e.cdp
        if os.environ.get("SHIM", "1") == "1":
            c.call("Page.addScriptToEvaluateOnNewDocument", {"source": SHIM_JS})
        t0 = time.time()
        c.call("Page.navigate", {"url": url})
        errors = []
        while time.time() - t0 < 1800:
            c.pump(5)
            errors += [l for l in console_text(c.take_events()) if l.startswith("EXCEPTION")]
            if errors:
                return {"status": "exception", "error": errors[0][:300]}
            st = json.loads(c.evaluate(
                "JSON.stringify({hash: location.hash, "
                "score: (document.querySelector('#result-number')||{}).textContent||''})"))
            if st["hash"] == "#summary" and st["score"].strip():
                vals = json.loads(c.evaluate(
                    "JSON.stringify(benchmarkClient._measuredValuesList.map("
                    "v => ({total: v.total, score: v.score, geomean: v.geomean, "
                    "tests: Object.fromEntries(Object.entries(v.tests).map(([k, t]) => [k, t.total]))})))"))
                return {"status": "done", "wall": time.time() - t0,
                        "page_score": st["score"].strip(), "iterations": vals}
        return {"status": "timeout"}


summary = {}
for rnd in range(1, rounds + 1):
    for eng, name, jsf in cfgs:
        r = run_once(eng, name, jsf, rnd)
        r.update({"label": label, "cfg": name, "engine": eng, "jsflags": jsf, "round": rnd,
                  "time": time.strftime("%H:%M:%S")})
        out.write(json.dumps(r) + "\n")
        out.flush()
        if r["status"] != "done":
            print("round %d %-10s %s %s" % (rnd, name, r["status"], r.get("error", "")), flush=True)
            continue
        totals = [it["total"] for it in r["iterations"]]
        summary.setdefault(name, []).extend(totals)
        print("round %d %-10s score=%-6s iteration totals(ms): %s" % (
            rnd, name, r["page_score"], " ".join("%.0f" % t for t in totals)), flush=True)

print()
base = None
for eng, name, jsf in cfgs:
    t = summary.get(name)
    if not t:
        continue
    med, mean = statistics.median(t), statistics.mean(t)
    sd = statistics.pstdev(t)
    if base is None:
        base = med
    print("%-10s n=%-3d median=%7.0f ms  mean=%7.0f ms  sd=%5.0f  vs first: %+.1f%% time" % (
        name, len(t), med, mean, sd, (med / base - 1) * 100))
