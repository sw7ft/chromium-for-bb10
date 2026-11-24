#!/usr/bin/env python3
"""Summarise a pageprof.py trace: profsum.py results/prof-<label>.json [min_task_ms]

Reports, for the renderer main thread: self time per event kind, script cost
per URL, and what each long task was made of.
"""
import collections
import json
import sys
from urllib.parse import urlparse

path = sys.argv[1]
min_task = float(sys.argv[2]) if len(sys.argv) > 2 else 500
doc = json.load(open(path))
ev = doc["traceEvents"]
meta = doc.get("berry", {})

names = {}
for e in ev:
    if e.get("ph") == "M" and e.get("name") == "thread_name":
        names[(e["pid"], e["tid"])] = e["args"].get("name", "")

# The renderer main thread is the one that runs script.
busy = collections.Counter()
for e in ev:
    if e.get("ph") == "X" and e.get("name") in ("EvaluateScript", "FunctionCall", "Layout",
                                                 "UpdateLayoutTree", "ParseHTML"):
        busy[(e["pid"], e["tid"])] += e.get("dur", 0)
if not busy:
    print("no renderer events; threads:", sorted(set(names.values())))
    sys.exit(1)
main = busy.most_common(1)[0][0]

# Complete events on the main thread (B/E pairs folded into X).
xs, open_b = [], []
for e in sorted((e for e in ev if (e.get("pid"), e.get("tid")) == main and "ts" in e),
                key=lambda e: e["ts"]):
    ph = e.get("ph")
    if ph == "X":
        xs.append(e)
    elif ph == "B":
        open_b.append(e)
    elif ph == "E" and open_b:
        b = open_b.pop()
        b = dict(b, ph="X", dur=e["ts"] - b["ts"])
        b.setdefault("args", {}).update(e.get("args", {}))
        xs.append(b)
xs.sort(key=lambda e: (e["ts"], -e.get("dur", 0)))


def url_of(e):
    a = e.get("args", {})
    d = a.get("data", a) if isinstance(a.get("data", a), dict) else {}
    u = d.get("url") or d.get("fileName") or a.get("fileName") or ""
    if not u:
        return ""
    p = urlparse(u)
    tail = p.path.rsplit("/", 1)[-1][:38]
    return "%s/%s" % (p.hostname or p.scheme, tail)


# Self time: an event's duration minus its direct children's.
self_by_name = collections.Counter()
count = collections.Counter()
script_by_url = collections.Counter()
compile_by_url = collections.Counter()
stack = []  # (end_ts, event, child_total)
tasks = []  # top-level events with a per-name self-time breakdown


def close(item):
    end, e, child, parts = item
    s = max(0, e.get("dur", 0) - child)
    self_by_name[e["name"]] += s
    count[e["name"]] += 1
    parts[e["name"]] += s
    return parts


for e in xs:
    while stack and stack[-1][0] <= e["ts"]:
        done = stack.pop()
        parts = close(done)
        if stack:
            stack[-1][3].update(parts)
        else:
            tasks.append((done[1], parts))
    if stack:
        stack[-1][2] += e.get("dur", 0)
    stack.append([e["ts"] + e.get("dur", 0), e, 0, collections.Counter()])
    n = e["name"]
    if n in ("EvaluateScript", "v8.run", "FunctionCall", "v8.callFunction"):
        pass
    if n in ("v8.compile", "V8.CompileCode", "v8.compileModule", "V8.CompileEval"):
        compile_by_url[url_of(e)] += e.get("dur", 0)
    if n == "EvaluateScript":
        script_by_url[url_of(e)] += e.get("dur", 0)
while stack:
    done = stack.pop()
    parts = close(done)
    if stack:
        stack[-1][3].update(parts)
    else:
        tasks.append((done[1], parts))

t_first = xs[0]["ts"]
t_last = max(e["ts"] + e.get("dur", 0) for e in xs)
total_busy = sum(e.get("dur", 0) for e, _ in tasks)
ms = lambda us: us / 1000.0

print("== %s ==" % path)
print("url=%s mode=%s jsflags=%s" % (meta.get("url"), meta.get("mode"), meta.get("jsflags")))
print("load complete %.1fs; main thread %s: span %.1fs, busy %.1fs in %d top-level tasks" % (
    meta.get("load_complete_s") or -1, names.get(main, main), ms(t_last - t_first) / 1000,
    ms(total_busy) / 1000, len(tasks)))
long_tasks = [(e, p) for e, p in tasks if ms(e.get("dur", 0)) >= min_task]
print("tasks >= %dms: %d, totalling %.1fs; longest %.1fs" % (
    min_task, len(long_tasks), ms(sum(e["dur"] for e, _ in long_tasks)) / 1000,
    ms(max([e.get("dur", 0) for e, _ in tasks] or [0])) / 1000))

print("\n-- main thread self time by event kind --")
for n, us in self_by_name.most_common(28):
    print("%8.0f ms %5.1f%%  %6d x  %s" % (ms(us), 100.0 * us / total_busy, count[n], n))

print("\n-- EvaluateScript total time by script (includes what it calls) --")
for u, us in script_by_url.most_common(12):
    print("%8.0f ms  %s" % (ms(us), u or "(inline)"))
if compile_by_url:
    print("\n-- compile time by script --")
    for u, us in compile_by_url.most_common(10):
        print("%8.0f ms  %s" % (ms(us), u or "(inline)"))

print("\n-- long tasks --")
for e, parts in sorted(long_tasks, key=lambda t: t[0]["ts"]):
    top = ", ".join("%s %.0f" % (n, ms(us)) for n, us in parts.most_common(6) if ms(us) >= 20)
    print("@%6.1fs %6.0f ms  %s" % (ms(e["ts"] - t_first) / 1000, ms(e["dur"]), top))

cache = collections.Counter(e["name"] for e in ev
                            if "ache" in e.get("name", "") and e.get("ph") in ("X", "I", "i", "B"))
if cache:
    print("\n-- cache-related events (all threads) --")
    for n, c in cache.most_common(14):
        print("%6d x  %s" % (c, n))
