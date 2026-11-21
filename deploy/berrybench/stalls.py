#!/usr/bin/env python3
"""Per-launch stall summary of berry-kbd.log: stalls.py <log>"""
import re, sys, collections
from urllib.parse import urlparse
L = open(sys.argv[1], errors="replace").read().split("\n")
starts = [i for i, l in enumerate(L) if "Berry Browser build" in l] + [len(L)]
ts = re.compile(r":(\d{4})/(\d{2})(\d{2})(\d{2})\.\d+:")
st = re.compile(r"MT stall=(\d+)ms gc=(\d+)ms/(\d+)x heap=(\d+)/(\d+)MB")
nav = re.compile(r"DidStartNav url=(\S+) same_doc=(\d)")
print("%-4s %-5s %-5s %5s %4s %6s %7s %6s %5s %5s %5s  %s" % ("ln", "build", "flag", "min", "navs", "stalls", "stall_s", "max_s", "gc%", "heap", "st/nav", "hosts"))
for a, b in zip(starts, starts[1:]):
    seg = L[a:b]
    build = re.search(r"build (\d+)", seg[0]).group(1)
    flag = any("no-optimize-for-size" in l for l in seg[:40])
    t = [int(m.group(2)) * 3600 + int(m.group(3)) * 60 + int(m.group(4)) for m in map(ts.search, seg) if m]
    dur = (max(t) - min(t)) / 60 if t else 0
    host = "?"; per = collections.Counter(); navs = 0; S = []
    for l in seg:
        m = nav.search(l)
        if m:
            h = urlparse(m.group(1)).hostname or "home"
            host = h.replace("www.", "").replace("m.", "", 1) if h else "home"
            navs += m.group(2) == "0"
        m = st.search(l)
        if m:
            S.append(tuple(map(int, m.groups())) + (host,)); per[host] += int(m.group(1))
    tot = sum(s[0] for s in S); gc = sum(s[1] for s in S)
    print("%-4d %-5s %-5s %5.1f %4d %6d %7.1f %6.1f %5.0f %5d %5.2f  %s" % (
        a + 1, build, "v8" if flag else "-", dur, navs, len(S), tot / 1000, max([s[0] for s in S] or [0]) / 1000,
        100 * gc / tot if tot else 0, max([s[4] for s in S] or [0]), len(S) / navs if navs else 0,
        ", ".join("%s %.0fs" % (h, v / 1000) for h, v in per.most_common(5))))
