#!/usr/bin/env python3
"""CPU and thread-state summary of pageprof.py runs: ttsum.py results/prof-<label>.json ...

For each run: CPU seconds per engine thread and per process over the measured
window, CPU used by the QNX process manager (memory mapping, page faults,
path lookups), and what the busiest engine thread (the page's main thread) was
doing in the state samples while the page was loading.
"""
import collections
import json
import os
import re
import sys

VERBOSE = os.environ.get("V", "1") == "1"


def threads(s):
    out = {}
    for l in s.split("\n"):
        m = re.match(r"\s*\d+\s+(\d+)\s+\S+\s+\S+\s+.*\s([\d.]+)\s*$", l)
        if m:  # idle threads print as "7m22s" and are skipped on purpose
            out[int(m.group(1))] = float(m.group(2))
    return out


def procs(s):
    out = {}
    for l in s.split("\n"):
        m = re.match(r"\s*(\d+)\s+(\S+)\s+\d+\s+\S+\s+\d+\s+[\d:]+\s+([\d.]+)\s+([\d.]+)", l)
        if m:
            out[(m.group(1), m.group(2))] = (float(m.group(3)), float(m.group(4)))
    return out


def part(s, key):
    if key == "engine":
        return s.split("==ALL==")[0]
    if key == "all":
        return s.split("==ALL==")[1].split("==PROCNTO==")[0]
    return s.split("==PROCNTO==")[1]


for path in sys.argv[1:]:
    b = json.load(open(path))["berry"]
    t0, t1 = b["ttimes"]
    a, z = threads(part(t0, "engine")), threads(part(t1, "engine"))
    d = sorted(((z[t] - a.get(t, 0), t) for t in z), reverse=True)
    tot = sum(x for x, _ in d)
    main_cpu, main = d[0]
    pa, pz = threads(part(t0, "procnto")), threads(part(t1, "procnto"))
    procnto = sum(pz[k] - pa.get(k, 0) for k in pz)

    # State samples of the main thread, from the first busy sample up to load
    # complete (sampling is uniform, so position in the file tracks time).
    blocks = open(path.replace(".json", ".samples")).read().split("==\n")
    states = []
    for blk in blocks:
        for l in blk.split("\n"):
            f = l.split()
            if f and f[0] == str(main):
                st = f[1]
                if st == "REPLY":
                    st += " " + (f[2] if len(f) > 2 else "?")
                states.append(st)
    first = next((i for i, s in enumerate(states) if s != "CONDVAR"), 0)
    lc = b.get("load_complete_s") or b["wall"]
    last = min(len(states), first + int(len(states) * lc / b["wall"]))
    win = collections.Counter(states[first:last])
    n = max(1, sum(win.values()))
    share = lambda k: 100.0 * win[k] / n
    other = 100 - share("RUNNING") - share("REPLY 1")
    print("%-14s env=%-28s load %5.1fs  main thread: cpu %5.1fs, running %4.1f%%, "
          "waiting on process manager %4.1f%%, other %4.1f%%  | all threads %.1fs, procnto %.1fs" % (
              b["label"], (b.get("engine_env") or "-")[:28], lc, main_cpu, share("RUNNING"),
              share("REPLY 1"), other, tot, procnto))
    if VERBOSE:
        print("   states during load (%d samples): %s" % (
            n, ", ".join("%s %.1f%%" % (k, 100.0 * v / n) for k, v in win.most_common(8))))
        print("   busiest engine threads (cpu s): %s" % ", ".join(
            "tid %d %.1f" % (t, x) for x, t in d[:6]))
        qa, qz = procs(part(t0, "all")), procs(part(t1, "all"))
        rows = sorted(((qz[k][0] - qa.get(k, (0, 0))[0] + qz[k][1] - qa.get(k, (0, 0))[1], k)
                       for k in qz), reverse=True)
        print("   cpu by process: %s" % ", ".join("%s %.1f" % (k[1].split("/")[-1], x)
                                                  for x, k in rows[:6]))
