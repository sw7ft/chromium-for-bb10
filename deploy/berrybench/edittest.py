#!/usr/bin/env python3
"""Copy/paste and "open from another app" test: edittest.py <engine>

Drives a bench engine on the phone the way a finger and the Navigator would,
through the engine's BERRY_TEST_CMD hook (content/shell/browser/
berry_test_hooks.h): touches enter Aura as platform touch events, so the long
press -> selection -> edit menu path is the real one, and invocations enter at
the same callback the Navigator's do.

The engine is pointed at a clipboard directory of its own (BERRY_CLIPBOARD_DIR)
so the phone's real clipboard is never read or replaced, and all host names
are made unresolvable, so nothing leaves the phone. Runs headless: nothing
appears on the phone's screen.
"""
import json
import re
import subprocess
import sys
import time
import urllib.parse

from engine import BENCH, PHONE, SSH, Engine, phone, stop_all

CMD = BENCH + "/data/test.cmd"
CLIP = BENCH + "/clip"
MENU_PNG = BENCH + "/data/editmenu.png"
CUT, COPY, PASTE, SELECT_ALL = 1, 2, 3, 4
NAMES = {CUT: "Cut", COPY: "Copy", PASTE: "Paste", SELECT_ALL: "Select all"}

HTML = """<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<style>body{font:20px sans-serif;margin:90px 20px 20px} p{margin:26px 0}
input,textarea{font:20px sans-serif;width:320px}</style>
<p id=p>Hello <span id=w>BlackBerry</span> world, this is text.</p>
<input id=i value="first second third">
<p><textarea id=t rows=2></textarea></p>
"""
PAGE = "data:text/html," + urllib.parse.quote(HTML)

results = []


def check(name, ok, detail=""):
    results.append(ok)
    print("%-4s %-46s %s" % ("ok" if ok else "FAIL", name, detail))
    return ok


def sh_quote(s):
    return "'" + s.replace("'", "'\\''") + "'"


def put(remote_cmd, data):
    """Run remote_cmd on the phone with data on its stdin (the phone's shell
    has no printf, and its echo rewrites backslashes)."""
    r = subprocess.run(SSH + [PHONE, remote_cmd], input=data.encode(),
                       capture_output=True, timeout=60)
    if r.returncode != 0:
        raise RuntimeError("%s: %s" % (remote_cmd, r.stderr.decode(errors="replace")))


def command(line, settle):
    """Hand one command to the engine (written aside, then renamed, so it is
    never read half-written) and give it time to act."""
    put("cat > %s.tmp && mv %s.tmp %s" % (CMD, CMD, CMD), line + "\n")
    time.sleep(settle)


def touch(x, y, hold_ms=60, settle=1.2):
    command("touch %d %d %d" % (x, y, hold_ms), hold_ms / 1000.0 + settle)


def long_press(x, y):
    touch(x, y, 900, 1.5)


def clip_text():
    r = phone("cat %s/text.plain 2>/dev/null" % CLIP)
    return r.stdout if r.returncode == 0 else None


def clip_set(text):
    """Another app copying: replace the system clipboard behind the engine."""
    put("on -u devuser /bin/sh -c %s" % sh_quote("cat > %s/text.plain" % CLIP), text)


class Log:
    def __init__(self, path):
        self.path, self.pos = path, 0

    def new(self):
        with open(self.path, errors="replace") as f:
            f.seek(self.pos)
            text = f.read()
            self.pos = f.tell()
        return text

    def menu(self):
        """Items of the menu opened since the last look: {command: (x, y)}."""
        opens = re.findall(r"BerryEdit: open bounds=([\d,-]+) anchor=[\d,-]+ items=(\S*)",
                           self.new())
        if not opens:
            return None, None
        bounds, items = opens[-1]
        out = {}
        for it in items.strip(";").split(";"):
            cid, xy = it.split("@")
            out[int(cid)] = tuple(int(v) for v in xy.split(","))
        return out, tuple(int(v) for v in bounds.split(","))


def center(c, sel, dx=0):
    x, y = json.loads(c.evaluate(
        "(function(){var r=document.querySelector(%s).getBoundingClientRect();"
        "return JSON.stringify([Math.round(r.left+r.width/2),Math.round(r.top+r.height/2)])})()"
        % json.dumps(sel)))
    return x + dx, y


def names(menu):
    return "[" + ", ".join(NAMES[k] for k in sorted(menu)) + "]" if menu else "no menu"


def pages(e):
    return [t["url"] for t in e.cdp.http_json("/json/list") if t.get("type") == "page"]


def main(eng):
    stop_all()
    phone("on -u devuser /bin/sh -c %s" % sh_quote(
        "mkdir -p %s; rm -f %s/* %s %s" % (CLIP, CLIP, CMD, MENU_PNG)))
    env = "BERRY_TEST_CMD=%s BERRY_CLIPBOARD_DIR=%s BERRY_EDIT_MENU_DUMP=%s QNX_NAV_DEBUG=1" % (
        CMD, CLIP, MENU_PNG)
    with Engine(eng, env=env, log_path="logs/edittest-%s.log" % eng.replace("/", "_"),
                extra="--content-shell-host-window-size=540x540 "
                      "'--host-resolver-rules=MAP * ^NOTFOUND, EXCLUDE 127.0.0.1'") as e:
        e.start()
        c, log = e.cdp, Log(e.log_path)
        copy_paste(c, log)
        c.call("Page.navigate", {"url": "about:blank"})
        c.pump(1.5)
        open_from_apps(e, log)
    ok = all(results)
    print("%d/%d passed" % (sum(results), len(results)))
    return 0 if ok else 1


def copy_paste(c, log):
    print("--- copy / paste ---")
    c.call("Page.navigate", {"url": PAGE})
    c.pump(2.5)
    log.new()

    # 1. Copy a word from the page.
    long_press(*center(c, "#w"))
    sel = c.evaluate("getSelection().toString()")
    check("long press selects the word", sel == "BlackBerry", repr(sel))
    menu, bounds = log.menu()
    check("menu offers Copy and Select all only",
          menu is not None and sorted(menu) == [COPY, SELECT_ALL], names(menu))
    if menu and COPY in menu:
        check("menu is clear of the toolbar strip and on screen",
              bounds[1] >= 56 and bounds[0] >= 0 and bounds[0] + bounds[2] <= 540
              and bounds[1] + bounds[3] <= 540, str(bounds))
        touch(*menu[COPY])
        check("Copy puts the word on the system clipboard", clip_text() == "BlackBerry",
              repr(clip_text()))
        check("menu closes after Copy", "BerryEdit: close" in log.new())

    # 2. Paste it into an empty text area.
    long_press(*center(c, "#t"))
    menu, _ = log.menu()
    check("long press in empty field offers Paste", menu is not None and PASTE in menu,
          names(menu))
    if menu and PASTE in menu:
        touch(*menu[PASTE])
        v = c.evaluate("document.getElementById('t').value")
        check("Paste inserts the copied word", v == "BlackBerry", repr(v))

    # 3. Text copied in another app replaces ours.
    clip_set("from another app \u00e9")
    long_press(*center(c, "#t", dx=120))
    menu, _ = log.menu()
    if check("menu again in the non-empty field", menu is not None and PASTE in menu,
             names(menu)):
        touch(*menu[PASTE])
        v = c.evaluate("document.getElementById('t').value")
        check("Paste inserts the other app's text", "from another app \u00e9" in v, repr(v))

    # 4. Select all, then Cut, in a single-line field.
    long_press(*center(c, "#i", dx=-110))
    sel = c.evaluate("(function(){var i=document.getElementById('i');"
                     "return i.value.substring(i.selectionStart,i.selectionEnd)})()")
    check("long press selects a word in an input", sel in ("first", "second", "third"),
          repr(sel))
    menu, _ = log.menu()
    check("menu offers Cut, Copy, Paste, Select all",
          menu is not None and sorted(menu) == [CUT, COPY, PASTE, SELECT_ALL], names(menu))
    phone("cp %s %s.full" % (MENU_PNG, MENU_PNG))  # keep the four-item menu to look at
    if menu and SELECT_ALL in menu:
        touch(*menu[SELECT_ALL], settle=1.8)
        sel = c.evaluate("(function(){var i=document.getElementById('i');"
                         "return i.value.substring(i.selectionStart,i.selectionEnd)})()")
        check("Select all selects the whole field", sel == "first second third", repr(sel))
        menu, _ = log.menu()
        check("menu returns for the new selection, without Select all",
              menu is not None and sorted(menu) == [CUT, COPY, PASTE], names(menu))
        if menu and CUT in menu:
            touch(*menu[CUT])
            v = c.evaluate("document.getElementById('i').value")
            check("Cut removes the text", v == "", repr(v))
            check("Cut puts it on the system clipboard",
                  clip_text() == "first second third", repr(clip_text()))

    # 5. A password must not be copyable.
    c.evaluate("(function(){var i=document.getElementById('i');i.type='password';"
               "i.value='hunter2 secret';})()")
    long_press(*center(c, "#i", dx=-110))
    menu, _ = log.menu()
    check("password field: no Cut or Copy",
          menu is None or (CUT not in menu and COPY not in menu), names(menu))
    touch(270, 520)  # tap empty page to dismiss


def open_from_apps(e, log):
    print("--- open from another app ---")
    base = "http://127.0.0.1:9222"
    check("one window with nothing in it", pages(e) == ["about:blank"], str(pages(e)))
    # A home-screen pin must not take over the window the user is in, even an
    # empty one (existing behaviour, kept).
    command("invoke berrybrowser://?u=%s&win=1"
            % urllib.parse.quote(base + "/json/protocol", safe=""), 3.0)
    p = pages(e)
    check("home-screen pin gets a window of its own",
          sorted(p) == ["about:blank", base + "/json/protocol"], str([u[-20:] for u in p]))
    # Build 112 only brought the empty window forward here and never loaded.
    command("invoke %s/json/version" % base, 2.5)
    p = pages(e)
    check("link from another app loads in the empty window",
          sorted(p) == [base + "/json/protocol", base + "/json/version"],
          str([u[-20:] for u in p]))
    command("invoke %s/json/version" % base, 2.0)
    check("same link again: no extra window", len(pages(e)) == 2, "%d windows" % len(pages(e)))
    command("invoke Have a look at this: %s/json/list. Nice!" % base, 2.5)
    p = pages(e)
    check("shared sentence: link found, opens new window",
          len(p) == 3 and base + "/json/list" in p, str([u[-20:] for u in p]))
    command("invoke BlackBerry Passport\\n%s/json" % base, 2.5)
    p = pages(e)
    check("shared title with the link on the next line",
          len(p) == 4 and base + "/json" in p, str([u[-20:] for u in p]))
    log.new()
    command("invoke passport keyboard shortcuts", 2.5)
    text = log.new()
    check("shared words become a search",
          'invoke dest="https://www.google.com/search?q=passport%20keyboard%20shortcuts"' in text,
          "".join(l for l in text.splitlines() if "invoke dest" in l)[-90:])
    command("invoke javascript://example.com/%0Aalert(1)", 2.0)
    text = log.new()
    check("a non-web address is searched for, not opened",
          'invoke dest="https://www.google.com/search?q=' in text
          and not any(u.startswith("javascript:") for u in pages(e)),
          "%d windows" % len(pages(e)))


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
