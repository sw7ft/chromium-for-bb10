#!/usr/bin/env python3
"""Run selftest.js on a bench engine: selftest.py <engine> [jsflags|-]"""
import json, sys
from engine import Engine, stop_all
eng = sys.argv[1]; jsf = sys.argv[2] if len(sys.argv) > 2 and sys.argv[2] != "-" else None
stop_all()
with Engine(eng, jsflags=jsf, log_path="logs/selftest-%s.log" % eng.replace("/", "_")) as e:
    e.start()
    r = json.loads(e.cdp.evaluate(open("selftest.js").read()))
    print("%s: math self-test %d/%d passed" % (eng, r["passed"], r["total"]))
    for f in r["failed"]:
        print("   FAIL", f)
    sys.exit(0 if r["passed"] == r["total"] else 1)
