#!/usr/bin/env python3
"""Start/stop a bench engine on the phone and attach to it over DevTools.

The engine runs as `devuser` from /accounts/devuser/berrybench (root may only
exec system binaries on this phone), headless by default so nothing appears on
the phone's screen. An SSH local forward exposes its DevTools port on the host.
"""
import os
import subprocess
import time

from cdp import CDP

PHONE = "passport-black-lan"
BENCH = "/accounts/devuser/berrybench"
SSH = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10"]
PORT = 9222


def phone(cmd, timeout=60):
    return subprocess.run(
        SSH + [PHONE, cmd], capture_output=True, text=True, timeout=timeout
    )


def stop_all():
    """Stop bench engines only. Never touches the installed content_shell.exe."""
    names = ("cs-o0", "cs-o2", "cs-fix", "cs-new")
    phone("; ".join("slay -f %s 2>/dev/null" % n for n in names) + "; sleep 1")
    phone("; ".join("slay -f -9 %s 2>/dev/null" % n for n in names) + "; true")


def wipe_profile():
    phone("rm -rf %s/data/profile %s/data/tmp %s/data/content_shell" % (BENCH, BENCH, BENCH))


class Engine:
    def __init__(self, engine, jsflags=None, ozone="headless", extra="", log_path=None,
                 env=""):
        self.engine, self.jsflags, self.ozone, self.extra = engine, jsflags, ozone, extra
        self.env = env
        self.log_path = log_path or os.path.join(
            os.path.dirname(os.path.abspath(__file__)), "logs",
            "%s-%d.log" % (engine.replace("/", "_"), int(time.time())))
        self.proc = None
        self.cdp = None

    def start(self, url="about:blank"):
        jsf = "'%s'" % self.jsflags if self.jsflags else "-"
        envs = ("env %s " % self.env) if self.env else ""
        remote = "on -u devuser %s/bin/sh %s/run.sh %s %s %s '%s' %s" % (
            envs, BENCH, self.engine, jsf, self.ozone, url, self.extra)
        self.log = open(self.log_path, "w")
        self.proc = subprocess.Popen(
            SSH + ["-o", "ExitOnForwardFailure=yes", "-L",
                   "%d:127.0.0.1:%d" % (PORT, PORT), PHONE, remote],
            stdout=self.log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        self.cdp = CDP("127.0.0.1", PORT, timeout=60)
        self.version = self.cdp.wait_ready(120)
        self.cdp.connect(self.cdp.page_ws_path())
        self.cdp.call("Runtime.enable")
        self.cdp.call("Page.enable")
        return self

    def stop(self):
        try:
            if self.cdp and self.cdp.sock:
                self.cdp.sock.close()
        except Exception:
            pass
        phone("slay -f %s 2>/dev/null; true" % self.engine.split("/")[-1])
        if self.proc:
            try:
                self.proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                phone("slay -f -9 %s 2>/dev/null; true" % self.engine.split("/")[-1])
                self.proc.terminate()
                try:
                    self.proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    self.proc.kill()
        if getattr(self, "log", None):
            self.log.close()

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.stop()


def console_text(events):
    """Flatten Runtime console/exception events into readable lines."""
    out = []
    for e in events:
        m = e.get("method")
        p = e.get("params", {})
        if m == "Runtime.consoleAPICalled":
            args = " ".join(
                str(a.get("value", a.get("description", ""))) for a in p.get("args", []))
            out.append("console.%s: %s" % (p.get("type"), args))
        elif m == "Runtime.exceptionThrown":
            d = p.get("exceptionDetails", {})
            desc = d.get("exception", {}).get("description") or d.get("text")
            out.append("EXCEPTION: %s @ %s:%s" % (
                desc, d.get("url", ""), d.get("lineNumber", "")))
    return out
