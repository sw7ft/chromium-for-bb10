import json, sys, time
from engine import Engine, console_text, stop_all
eng = sys.argv[1]; jsf = sys.argv[2] if len(sys.argv)>2 and sys.argv[2] != "-" else None
exprs = [l.rstrip("\n") for l in open(sys.argv[3]) if l.strip() and not l.startswith("#")]
url = sys.argv[4] if len(sys.argv)>4 else "https://browserbench.org/Speedometer3.0/resources/todomvc/architecture-examples/backbone/dist/index.html"
stop_all()
with Engine(eng, jsflags=jsf, log_path="logs/bbdebug.log", extra="--content-shell-host-window-size=540x540") as e:
    e.start(); c=e.cdp
    c.call("Page.navigate", {"url":url}); c.pump(7)
    for l in console_text(c.take_events()): print("load:", l[:300])
    for x in exprs:
        try: r = c.evaluate(x)
        except Exception as ex: r = "ERR "+str(ex)[:400]
        print("%-90s => %s" % (x[:90], json.dumps(r)[:500]))
